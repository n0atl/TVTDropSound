// DropSound - ドロップしたら音を鳴らす TVTest プラグイン

#include <windows.h>
#include <commdlg.h>
#include <mmsystem.h>
#include <string>
#include <cmath>
#define TVTEST_PLUGIN_CLASS_IMPLEMENT
#include "TVTestPlugin.h"
#include "resource.h"
#include "PidDropCounter.h"

#pragma comment(lib, "winmm.lib")
#pragma comment(lib, "comdlg32.lib")
#pragma comment(lib, "advapi32.lib")


namespace
{

const UINT TIMER_ID_CHECK = 1;
const UINT CHECK_INTERVAL_MS = 500;
const LPCWSTR WINDOW_CLASS_NAME = L"TVTest DropSound Window";
// 前の音が鳴り終わる前に次の音を重ねて鳴らせるよう、複数のデバイスを順番に使う
const int MCI_SLOTS = 8;

enum {
	MODE_SYSTEM,
	MODE_FILE
};

struct SystemSoundInfo
{
	LPCWSTR pszAlias;
	LPCWSTR pszName;
};

const SystemSoundInfo SYSTEM_SOUNDS[] = {
	{L"SystemExclamation",    L"警告"},
	{L"SystemHand",           L"システムエラー"},
	{L"SystemAsterisk",       L"メッセージ(情報)"},
	{L".Default",             L"一般の警告音"},
	{L"SystemNotification",   L"システム通知"},
	{L"Notification.Default", L"通知"},
};
const int NUM_SYSTEM_SOUNDS = _countof(SYSTEM_SOUNDS);

struct Settings
{
	int Mode = MODE_SYSTEM;
	int SystemSound = 0;
	std::wstring FilePath;
	int Volume = 100;     // 0-100
	bool EveryChange = false;  // 間隔を空けず、ドロップ数が増えるたびに鳴らす
	int IntervalSec = 2;       // 鳴らす最小間隔(秒)
	bool OnlyAV = false;       // 映像・音声の PID でドロップしたときだけ鳴らす
	bool PitchUp = true;       // 増加量が多いほどピッチを上げる
	int PitchThreshold = 100;  // ピッチを上げ始める 1 秒あたりの増加量
};

int Clamp(int Value, int Min, int Max)
{
	return Value < Min ? Min : Value > Max ? Max : Value;
}

// 1 秒あたりのドロップの増加量から再生速度(1000 = 等速)を決める
// タイマーの間隔のずれに左右されないよう、1 秒あたりに換算した値で判定する
// しきい値で 2 半音上がり、増加量が 10 倍になるごとにさらに 4 半音上がる(最大 1 オクターブ)
int GetPlaybackSpeed(const Settings &s, double IncreasePerSec)
{
	if (!s.PitchUp || IncreasePerSec < s.PitchThreshold)
		return 1000;
	double Semitones = 2.0 + 4.0 * std::log10(IncreasePerSec / s.PitchThreshold);
	if (Semitones > 12.0)
		Semitones = 12.0;
	return (int)(1000.0 * std::pow(2.0, Semitones / 12.0) + 0.5);
}

// システム音に割り当てられている WAV ファイルのパスを取得する
std::wstring GetSystemSoundPath(LPCWSTR pszAlias)
{
	std::wstring Key = std::wstring(L"AppEvents\\Schemes\\Apps\\.Default\\") + pszAlias + L"\\.Current";
	WCHAR szPath[MAX_PATH] = {};
	DWORD Size = sizeof(szPath);
	if (::RegGetValueW(HKEY_CURRENT_USER, Key.c_str(), nullptr, RRF_RT_REG_SZ, nullptr, szPath, &Size) != ERROR_SUCCESS
			|| szPath[0] == L'\0')
		return std::wstring();

	// ファイル名だけの場合は Windows\Media にあるものとみなす
	if (::wcschr(szPath, L'\\') == nullptr) {
		WCHAR szMedia[MAX_PATH];
		::GetWindowsDirectoryW(szMedia, MAX_PATH);
		return std::wstring(szMedia) + L"\\Media\\" + szPath;
	}
	return szPath;
}

}


class CDropSound : public TVTest::CTVTestPlugin
{
public:
	bool GetPluginInfo(TVTest::PluginInfo *pInfo) override;
	bool Initialize() override;
	bool Finalize() override;

private:
	Settings m_Settings;
	std::wstring m_IniPath;
	HWND m_hwnd = nullptr;

	bool m_fHasBaseline = false;
	DWORD m_LastDropCount = 0;
	DWORD m_LastCheckTick = 0;
	bool m_fPlayed = false;
	DWORD m_LastPlayTick = 0;

	CPidDropCounter m_AVDropCounter;
	bool m_fStreamCallback = false;

	bool m_fMciOpen[MCI_SLOTS] = {};
	int m_NextMciSlot = 0;
	std::wstring m_MciFilePath;

	Settings m_DialogSettings;

	void LoadSettings();
	void SaveSettings();
	void Start();
	void Stop();
	void ResetBaseline() { m_fHasBaseline = false; }
	void CheckDrop();
	void UpdateStreamCallback(bool fEnabled);
	void UpdateAVPids();
	void PlaySoundBySettings(const Settings &s, int Speed = 1000);
	bool PlayMci(const std::wstring &Path, int Volume, int Speed);
	void CloseMci();
	bool ShowSettingsDialog(HWND hwndOwner);
	void UpdateDialogControls(HWND hDlg);

	static LRESULT CALLBACK EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void *pClientData);
	static BOOL CALLBACK StreamCallback(BYTE *pData, void *pClientData);
	static LRESULT CALLBACK WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam);
	static INT_PTR CALLBACK DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam);
};


bool CDropSound::GetPluginInfo(TVTest::PluginInfo *pInfo)
{
	pInfo->Type           = TVTest::PLUGIN_TYPE_NORMAL;
	pInfo->Flags          = TVTest::PLUGIN_FLAG_HASSETTINGS;
	pInfo->pszPluginName  = L"DropSound";
	pInfo->pszCopyright   = L"(C) 2026 n0atl / MIT License";
	pInfo->pszDescription = L"ドロップしたら音を鳴らします。";
	return true;
}


bool CDropSound::Initialize()
{
	WCHAR szPath[MAX_PATH];
	DWORD Length = ::GetModuleFileNameW(g_hinstDLL, szPath, MAX_PATH);
	m_IniPath.assign(szPath, Length);
	std::wstring::size_type Ext = m_IniPath.find_last_of(L'.');
	if (Ext != std::wstring::npos)
		m_IniPath.erase(Ext);
	m_IniPath += L".ini";
	LoadSettings();

	WNDCLASSW wc = {};
	wc.lpfnWndProc = WndProc;
	wc.hInstance = g_hinstDLL;
	wc.lpszClassName = WINDOW_CLASS_NAME;
	if (!::RegisterClassW(&wc))
		return false;
	m_hwnd = ::CreateWindowExW(0, WINDOW_CLASS_NAME, nullptr, 0, 0, 0, 0, 0,
	                           HWND_MESSAGE, nullptr, g_hinstDLL, this);
	if (m_hwnd == nullptr)
		return false;

	m_pApp->SetEventCallback(EventCallback, this);

	if (m_pApp->IsPluginEnabled())
		Start();

	return true;
}


bool CDropSound::Finalize()
{
	Stop();
	if (m_hwnd != nullptr) {
		::DestroyWindow(m_hwnd);
		m_hwnd = nullptr;
	}
	::UnregisterClassW(WINDOW_CLASS_NAME, g_hinstDLL);
	return true;
}


void CDropSound::LoadSettings()
{
	LPCWSTR pszIni = m_IniPath.c_str();
	m_Settings.Mode = Clamp(::GetPrivateProfileIntW(L"Settings", L"Mode", MODE_SYSTEM, pszIni), MODE_SYSTEM, MODE_FILE);
	m_Settings.SystemSound = Clamp(::GetPrivateProfileIntW(L"Settings", L"SystemSound", 0, pszIni), 0, NUM_SYSTEM_SOUNDS - 1);
	WCHAR szPath[MAX_PATH] = {};
	::GetPrivateProfileStringW(L"Settings", L"FilePath", L"", szPath, MAX_PATH, pszIni);
	m_Settings.FilePath = szPath;
	m_Settings.Volume = Clamp(::GetPrivateProfileIntW(L"Settings", L"Volume", 100, pszIni), 0, 100);
	m_Settings.EveryChange = ::GetPrivateProfileIntW(L"Settings", L"EveryChange", 0, pszIni) != 0;
	m_Settings.IntervalSec = Clamp(::GetPrivateProfileIntW(L"Settings", L"IntervalSec", 2, pszIni), 1, 3600);
	m_Settings.OnlyAV = ::GetPrivateProfileIntW(L"Settings", L"OnlyAV", 0, pszIni) != 0;
	m_Settings.PitchUp = ::GetPrivateProfileIntW(L"Settings", L"PitchUp", 1, pszIni) != 0;
	m_Settings.PitchThreshold = Clamp(::GetPrivateProfileIntW(L"Settings", L"PitchThreshold", 100, pszIni), 1, 100000000);
}


void CDropSound::SaveSettings()
{
	LPCWSTR pszIni = m_IniPath.c_str();

	// 日本語のファイルパスを保存できるように、INI ファイルを UTF-16 で作成しておく
	if (::GetFileAttributesW(pszIni) == INVALID_FILE_ATTRIBUTES) {
		HANDLE hFile = ::CreateFileW(pszIni, GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
		if (hFile != INVALID_HANDLE_VALUE) {
			static const BYTE BOM[] = {0xFF, 0xFE};
			DWORD Written;
			::WriteFile(hFile, BOM, sizeof(BOM), &Written, nullptr);
			::CloseHandle(hFile);
		}
	}

	::WritePrivateProfileStringW(L"Settings", L"Mode", std::to_wstring(m_Settings.Mode).c_str(), pszIni);
	::WritePrivateProfileStringW(L"Settings", L"SystemSound", std::to_wstring(m_Settings.SystemSound).c_str(), pszIni);
	::WritePrivateProfileStringW(L"Settings", L"FilePath", m_Settings.FilePath.c_str(), pszIni);
	::WritePrivateProfileStringW(L"Settings", L"Volume", std::to_wstring(m_Settings.Volume).c_str(), pszIni);
	::WritePrivateProfileStringW(L"Settings", L"EveryChange", m_Settings.EveryChange ? L"1" : L"0", pszIni);
	::WritePrivateProfileStringW(L"Settings", L"IntervalSec", std::to_wstring(m_Settings.IntervalSec).c_str(), pszIni);
	::WritePrivateProfileStringW(L"Settings", L"OnlyAV", m_Settings.OnlyAV ? L"1" : L"0", pszIni);
	::WritePrivateProfileStringW(L"Settings", L"PitchUp", m_Settings.PitchUp ? L"1" : L"0", pszIni);
	::WritePrivateProfileStringW(L"Settings", L"PitchThreshold", std::to_wstring(m_Settings.PitchThreshold).c_str(), pszIni);
}


void CDropSound::Start()
{
	ResetBaseline();
	m_fPlayed = false;
	// EVENT_PLUGINENABLE の時点ではまだ有効状態が変わっていないので、有効として扱う
	UpdateStreamCallback(true);
	::SetTimer(m_hwnd, TIMER_ID_CHECK, CHECK_INTERVAL_MS, nullptr);
}


void CDropSound::Stop()
{
	if (m_hwnd != nullptr)
		::KillTimer(m_hwnd, TIMER_ID_CHECK);
	if (m_fStreamCallback) {
		m_pApp->SetStreamCallback(TVTest::STREAM_CALLBACK_REMOVE, StreamCallback, this);
		m_fStreamCallback = false;
	}
	CloseMci();
}


// 映像・音声だけを対象にするときは、TS パケットを受け取って自前でドロップを数える
// (TVTest から取得できるドロップ数は全 PID の合計のため)
void CDropSound::UpdateStreamCallback(bool fEnabled)
{
	const bool fNeed = m_Settings.OnlyAV && fEnabled;
	if (fNeed == m_fStreamCallback)
		return;

	if (fNeed) {
		UpdateAVPids();
		m_fStreamCallback = m_pApp->SetStreamCallback(0, StreamCallback, this);
	} else {
		m_pApp->SetStreamCallback(TVTest::STREAM_CALLBACK_REMOVE, StreamCallback, this);
		m_fStreamCallback = false;
	}
}


// 視聴中のサービスの映像・音声 PID を対象にする
void CDropSound::UpdateAVPids()
{
	m_AVDropCounter.ClearTargets();

	const int Service = m_pApp->GetService();
	TVTest::ServiceInfo Info = {};
	if (Service < 0 || !m_pApp->GetServiceInfo(Service, &Info))
		return;

	if (Info.VideoPID != 0)
		m_AVDropCounter.AddTarget(Info.VideoPID);
	for (int i = 0; i < Info.NumAudioPIDs && i < _countof(Info.AudioPID); i++)
		m_AVDropCounter.AddTarget(Info.AudioPID[i]);
}


BOOL CALLBACK CDropSound::StreamCallback(BYTE *pData, void *pClientData)
{
	static_cast<CDropSound*>(pClientData)->m_AVDropCounter.InputPacket(pData);
	return TRUE;
}


void CDropSound::CheckDrop()
{
	DWORD DropCount;
	if (m_fStreamCallback) {
		DropCount = m_AVDropCounter.GetDropCount();
	} else {
		TVTest::StatusInfo Status = {};
		Status.Size = sizeof(Status);
		if (!m_pApp->GetStatus(&Status))
			return;
		DropCount = Status.DropPacketCount;
	}
	const DWORD Now = ::GetTickCount();

	// 初回やリセット後は現在値を基準にするだけ
	if (!m_fHasBaseline || DropCount < m_LastDropCount) {
		m_LastDropCount = DropCount;
		m_LastCheckTick = Now;
		m_fHasBaseline = true;
		return;
	}

	if (DropCount > m_LastDropCount) {
		if (m_Settings.EveryChange || !m_fPlayed
				|| Now - m_LastPlayTick >= (DWORD)m_Settings.IntervalSec * 1000) {
			const DWORD Elapsed = Now - m_LastCheckTick;
			const double IncreasePerSec = (double)(DropCount - m_LastDropCount) * 1000.0 / (Elapsed > 0 ? Elapsed : 1);
			PlaySoundBySettings(m_Settings, GetPlaybackSpeed(m_Settings, IncreasePerSec));
			m_LastPlayTick = Now;
			m_fPlayed = true;
		}
	}

	m_LastDropCount = DropCount;
	m_LastCheckTick = Now;
}


void CDropSound::PlaySoundBySettings(const Settings &s, int Speed)
{
	if (s.Mode == MODE_FILE) {
		if (s.FilePath.empty() || !PlayMci(s.FilePath, s.Volume, Speed)) {
			// ファイルを再生できなかった場合はシステム音で代用する
			::MessageBeep(MB_ICONEXCLAMATION);
		}
		return;
	}

	LPCWSTR pszAlias = SYSTEM_SOUNDS[Clamp(s.SystemSound, 0, NUM_SYSTEM_SOUNDS - 1)].pszAlias;
	std::wstring Path = GetSystemSoundPath(pszAlias);
	if (Path.empty() || !PlayMci(Path, s.Volume, Speed)) {
		// PlaySound は前の音を止めてしまうので、重ねて鳴らせない
		::PlaySoundW(pszAlias, nullptr, SND_ALIAS | SND_ASYNC);
	}
}


bool CDropSound::PlayMci(const std::wstring &Path, int Volume, int Speed)
{
	if (m_MciFilePath != Path)
		CloseMci();

	const int Slot = m_NextMciSlot;
	m_NextMciSlot = (m_NextMciSlot + 1) % MCI_SLOTS;
	const std::wstring Alias = L"TVTDropSound" + std::to_wstring(Slot);

	if (!m_fMciOpen[Slot]) {
		// mpegvideo (DirectShow) を使うと WAV / MP3 / WMA などを同じ方法で再生できる
		std::wstring Command = L"open \"" + Path + L"\" type mpegvideo alias " + Alias;
		MCIERROR Error = ::mciSendStringW(Command.c_str(), nullptr, 0, nullptr);
		if (Error != 0) {
			WCHAR szError[256] = {};
			::mciGetErrorStringW(Error, szError, _countof(szError));
			std::wstring Log = L"音声ファイルを開けません: " + Path + L" (" + szError + L")";
			m_pApp->AddLog(Log.c_str(), TVTest::LOG_TYPE_ERROR);
			return false;
		}
		m_fMciOpen[Slot] = true;
		m_MciFilePath = Path;
	}

	std::wstring Command = L"setaudio " + Alias + L" volume to " + std::to_wstring(Volume * 10);
	::mciSendStringW(Command.c_str(), nullptr, 0, nullptr);
	// 速く再生すると音が高くなる(そのぶん短くもなる)
	Command = L"set " + Alias + L" speed " + std::to_wstring(Speed);
	::mciSendStringW(Command.c_str(), nullptr, 0, nullptr);
	Command = L"play " + Alias + L" from 0";
	return ::mciSendStringW(Command.c_str(), nullptr, 0, nullptr) == 0;
}


void CDropSound::CloseMci()
{
	for (int i = 0; i < MCI_SLOTS; i++) {
		if (m_fMciOpen[i]) {
			std::wstring Command = L"close TVTDropSound" + std::to_wstring(i);
			::mciSendStringW(Command.c_str(), nullptr, 0, nullptr);
			m_fMciOpen[i] = false;
		}
	}
	m_NextMciSlot = 0;
	m_MciFilePath.clear();
}


LRESULT CALLBACK CDropSound::EventCallback(UINT Event, LPARAM lParam1, LPARAM lParam2, void *pClientData)
{
	CDropSound *pThis = static_cast<CDropSound*>(pClientData);

	switch (Event) {
	case TVTest::EVENT_PLUGINENABLE:
		if (lParam1 != 0)
			pThis->Start();
		else
			pThis->Stop();
		return TRUE;

	case TVTest::EVENT_PLUGINSETTINGS:
		return pThis->ShowSettingsDialog(reinterpret_cast<HWND>(lParam1));

	case TVTest::EVENT_CHANNELCHANGE:
	case TVTest::EVENT_SERVICECHANGE:
	case TVTest::EVENT_DRIVERCHANGE:
		if (pThis->m_fStreamCallback)
			pThis->UpdateAVPids();
		pThis->ResetBaseline();
		return TRUE;

	case TVTest::EVENT_SERVICEUPDATE:
		// PMT の更新などで映像・音声の PID が変わることがある
		if (pThis->m_fStreamCallback)
			pThis->UpdateAVPids();
		return TRUE;

	case TVTest::EVENT_STATUSRESET:
		pThis->ResetBaseline();
		return TRUE;
	}

	return 0;
}


LRESULT CALLBACK CDropSound::WndProc(HWND hwnd, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {
	case WM_CREATE:
		{
			LPCREATESTRUCT pcs = reinterpret_cast<LPCREATESTRUCT>(lParam);
			::SetWindowLongPtrW(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(pcs->lpCreateParams));
		}
		return 0;

	case WM_TIMER:
		if (wParam == TIMER_ID_CHECK) {
			CDropSound *pThis = reinterpret_cast<CDropSound*>(::GetWindowLongPtrW(hwnd, GWLP_USERDATA));
			if (pThis != nullptr)
				pThis->CheckDrop();
		}
		return 0;
	}

	return ::DefWindowProcW(hwnd, uMsg, wParam, lParam);
}


bool CDropSound::ShowSettingsDialog(HWND hwndOwner)
{
	m_DialogSettings = m_Settings;
	if (::DialogBoxParamW(g_hinstDLL, MAKEINTRESOURCEW(IDD_SETTINGS), hwndOwner,
	                      DlgProc, reinterpret_cast<LPARAM>(this)) != IDOK)
		return false;

	m_Settings = m_DialogSettings;
	SaveSettings();
	// 数えるドロップの種類が変わる場合があるので、基準を取り直す
	UpdateStreamCallback(m_pApp->IsPluginEnabled());
	ResetBaseline();
	return true;
}


void CDropSound::UpdateDialogControls(HWND hDlg)
{
	const bool fFile = ::IsDlgButtonChecked(hDlg, IDC_MODE_FILE) == BST_CHECKED;
	::EnableWindow(::GetDlgItem(hDlg, IDC_SYSTEM_SOUND), !fFile);
	::EnableWindow(::GetDlgItem(hDlg, IDC_FILE_PATH), fFile);
	::EnableWindow(::GetDlgItem(hDlg, IDC_BROWSE), fFile);
	::EnableWindow(::GetDlgItem(hDlg, IDC_INTERVAL),
	               ::IsDlgButtonChecked(hDlg, IDC_TIMING_INTERVAL) == BST_CHECKED);
	::EnableWindow(::GetDlgItem(hDlg, IDC_PITCH_THRESHOLD),
	               ::IsDlgButtonChecked(hDlg, IDC_PITCH) == BST_CHECKED);
}


static Settings GetDialogSettings(HWND hDlg)
{
	Settings s;
	s.Mode = ::IsDlgButtonChecked(hDlg, IDC_MODE_FILE) == BST_CHECKED ? MODE_FILE : MODE_SYSTEM;
	s.SystemSound = Clamp((int)::SendDlgItemMessageW(hDlg, IDC_SYSTEM_SOUND, CB_GETCURSEL, 0, 0), 0, NUM_SYSTEM_SOUNDS - 1);
	WCHAR szPath[MAX_PATH] = {};
	::GetDlgItemTextW(hDlg, IDC_FILE_PATH, szPath, MAX_PATH);
	s.FilePath = szPath;
	s.Volume = Clamp(::GetDlgItemInt(hDlg, IDC_VOLUME, nullptr, FALSE), 0, 100);
	s.EveryChange = ::IsDlgButtonChecked(hDlg, IDC_TIMING_EVERY) == BST_CHECKED;
	s.IntervalSec = Clamp(::GetDlgItemInt(hDlg, IDC_INTERVAL, nullptr, FALSE), 1, 3600);
	s.OnlyAV = ::IsDlgButtonChecked(hDlg, IDC_ONLY_AV) == BST_CHECKED;
	s.PitchUp = ::IsDlgButtonChecked(hDlg, IDC_PITCH) == BST_CHECKED;
	s.PitchThreshold = Clamp(::GetDlgItemInt(hDlg, IDC_PITCH_THRESHOLD, nullptr, FALSE), 1, 100000000);
	return s;
}


INT_PTR CALLBACK CDropSound::DlgProc(HWND hDlg, UINT uMsg, WPARAM wParam, LPARAM lParam)
{
	switch (uMsg) {
	case WM_INITDIALOG:
		{
			CDropSound *pThis = reinterpret_cast<CDropSound*>(lParam);
			::SetWindowLongPtrW(hDlg, DWLP_USER, lParam);
			const Settings &s = pThis->m_DialogSettings;

			::CheckRadioButton(hDlg, IDC_MODE_SYSTEM, IDC_MODE_FILE,
			                   s.Mode == MODE_FILE ? IDC_MODE_FILE : IDC_MODE_SYSTEM);
			for (const SystemSoundInfo &Info : SYSTEM_SOUNDS)
				::SendDlgItemMessageW(hDlg, IDC_SYSTEM_SOUND, CB_ADDSTRING, 0, reinterpret_cast<LPARAM>(Info.pszName));
			::SendDlgItemMessageW(hDlg, IDC_SYSTEM_SOUND, CB_SETCURSEL, s.SystemSound, 0);
			::SetDlgItemTextW(hDlg, IDC_FILE_PATH, s.FilePath.c_str());
			::SetDlgItemInt(hDlg, IDC_VOLUME, s.Volume, FALSE);
			::CheckRadioButton(hDlg, IDC_TIMING_EVERY, IDC_TIMING_INTERVAL,
			                   s.EveryChange ? IDC_TIMING_EVERY : IDC_TIMING_INTERVAL);
			::SetDlgItemInt(hDlg, IDC_INTERVAL, s.IntervalSec, FALSE);
			::CheckDlgButton(hDlg, IDC_ONLY_AV, s.OnlyAV ? BST_CHECKED : BST_UNCHECKED);
			::CheckDlgButton(hDlg, IDC_PITCH, s.PitchUp ? BST_CHECKED : BST_UNCHECKED);
			::SetDlgItemInt(hDlg, IDC_PITCH_THRESHOLD, s.PitchThreshold, FALSE);
			::SetDlgItemInt(hDlg, IDC_TEST_INCREASE, 1, FALSE);
			pThis->UpdateDialogControls(hDlg);
		}
		return TRUE;

	case WM_COMMAND:
		{
			CDropSound *pThis = reinterpret_cast<CDropSound*>(::GetWindowLongPtrW(hDlg, DWLP_USER));

			switch (LOWORD(wParam)) {
			case IDC_MODE_SYSTEM:
			case IDC_MODE_FILE:
			case IDC_TIMING_EVERY:
			case IDC_TIMING_INTERVAL:
			case IDC_PITCH:
				pThis->UpdateDialogControls(hDlg);
				return TRUE;

			case IDC_BROWSE:
				{
					WCHAR szFile[MAX_PATH] = {};
					::GetDlgItemTextW(hDlg, IDC_FILE_PATH, szFile, MAX_PATH);
					OPENFILENAMEW ofn = {};
					ofn.lStructSize = sizeof(ofn);
					ofn.hwndOwner = hDlg;
					ofn.lpstrFilter =
						L"音声ファイル (*.wav;*.mp3;*.wma;*.m4a;*.aac;*.flac)\0*.wav;*.mp3;*.wma;*.m4a;*.aac;*.flac\0"
						L"すべてのファイル (*.*)\0*.*\0";
					ofn.lpstrFile = szFile;
					ofn.nMaxFile = MAX_PATH;
					ofn.lpstrTitle = L"鳴らす音声ファイルを選択";
					ofn.Flags = OFN_FILEMUSTEXIST | OFN_PATHMUSTEXIST | OFN_HIDEREADONLY | OFN_NOCHANGEDIR;
					if (::GetOpenFileNameW(&ofn))
						::SetDlgItemTextW(hDlg, IDC_FILE_PATH, szFile);
				}
				return TRUE;

			case IDC_TEST:
				{
					// 「増加量」に入力した数だけ 1 秒あたりにドロップが増えたときの音を鳴らす
					const Settings s = GetDialogSettings(hDlg);
					const UINT Increase = ::GetDlgItemInt(hDlg, IDC_TEST_INCREASE, nullptr, FALSE);
					pThis->PlaySoundBySettings(s, GetPlaybackSpeed(s, Increase));
				}
				return TRUE;

			case IDOK:
				pThis->m_DialogSettings = GetDialogSettings(hDlg);
				[[fallthrough]];
			case IDCANCEL:
				::EndDialog(hDlg, LOWORD(wParam));
				return TRUE;
			}
		}
		break;
	}

	return FALSE;
}


TVTest::CTVTestPlugin *CreatePluginClass()
{
	return new CDropSound;
}
