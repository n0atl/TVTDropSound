// 指定した PID だけを対象に、TS パケットの連続性カウンタからドロップを数える

#pragma once

#include <windows.h>
#include <atomic>
#include <cstring>


class CPidDropCounter
{
public:
	static const int PID_COUNT = 0x2000;

	CPidDropCounter()
	{
		for (auto &Target : m_Target)
			Target.store(false, std::memory_order_relaxed);
		std::memset(m_LastCC, CC_UNKNOWN, sizeof(m_LastCC));
	}

	// 以下はメインスレッドから呼ぶ

	void ClearTargets()
	{
		for (auto &Target : m_Target)
			Target.store(false, std::memory_order_relaxed);
		m_fResetRequest.store(true);
	}

	void AddTarget(WORD PID)
	{
		if (PID < PID_COUNT)
			m_Target[PID].store(true, std::memory_order_relaxed);
	}

	DWORD GetDropCount() const { return m_DropCount.load(); }

	// 以下はストリームのスレッドから呼ぶ

	void InputPacket(const BYTE *pData)
	{
		if (m_fResetRequest.exchange(false))
			std::memset(m_LastCC, CC_UNKNOWN, sizeof(m_LastCC));

		// 同期バイトが違うものや、エラーのあるパケットはドロップの判定に使わない
		if (pData[0] != 0x47 || (pData[1] & 0x80) != 0)
			return;

		const WORD PID = ((pData[1] & 0x1F) << 8) | pData[2];
		if (!m_Target[PID].load(std::memory_order_relaxed))
			return;

		const BYTE AdaptationFieldControl = (pData[3] >> 4) & 0x03;
		const BYTE CC = pData[3] & 0x0F;

		// ペイロードの無いパケットではカウンタが進まない
		if ((AdaptationFieldControl & 0x01) == 0)
			return;

		// discontinuity_indicator が立っている場合は不連続でもドロップではない
		const bool fDiscontinuity =
			(AdaptationFieldControl & 0x02) != 0 && pData[4] > 0 && (pData[5] & 0x80) != 0;

		const BYTE LastCC = m_LastCC[PID];
		if (LastCC != CC_UNKNOWN && !fDiscontinuity
				&& CC != LastCC                      // 同じ値は重送パケットとして許容される
				&& CC != ((LastCC + 1) & 0x0F))
			m_DropCount.fetch_add(1);

		m_LastCC[PID] = CC;
	}

private:
	static const BYTE CC_UNKNOWN = 0xFF;

	std::atomic<bool> m_Target[PID_COUNT];
	std::atomic<bool> m_fResetRequest{false};
	std::atomic<DWORD> m_DropCount{0};
	BYTE m_LastCC[PID_COUNT];
};
