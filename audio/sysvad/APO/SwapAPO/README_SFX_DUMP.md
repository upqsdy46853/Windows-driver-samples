# SwapAPO SFX Dump 使用注意事項

這份 README 是給目前這個 debug 版 `SwapAPO` 使用的。它會在 SFX APO 裡把處理前與處理後的 audio buffer dump 成 WAV，方便比對 swap 前後的資料。

## 輸出位置

WAV 會輸出到：

```bat
C:\SwapApoDll
```

檔名格式大概會像：

```text
SwapAPOSFX_Stream2_QPC7024880387770_IN.wav
SwapAPOSFX_Stream2_QPC7024880387770_OUT.wav
```

`IN` 是 SFX 處理前的 buffer，`OUT` 是 SFX 處理後的 buffer。

## 第一次使用前要建立資料夾

`SwapAPO.dll` 是被 `audiodg.exe` 載入，不是由一般使用者程式寫檔。建議先用系統管理員開 `cmd`，建立資料夾並給 Local Service 寫入權限：

```bat
mkdir C:\SwapApoDll
icacls C:\SwapApoDll /grant "*S-1-5-19:(OI)(CI)(M)"
```

`*S-1-5-19` 是 `LOCAL SERVICE` 的 SID。用 SID 比帳號名稱穩，因為不同語言的 Windows 顯示名稱可能不一樣。

如果 log 裡看到 `DumpInReady=1` / `DumpOutReady=1`，但是資料夾裡沒有 WAV，優先檢查這個資料夾權限。

## Build

在 EWDK 掛載於 `D:` 的情況下，可以從一般 PowerShell/cmd 跑：

```bat
cd /d C:\Users\26100\Desktop\Windows-driver-samples\audio\sysvad
msbuild APO\SwapAPO\SwapAPO.vcxproj /t:Clean /p:Configuration=Debug /p:Platform=x64
msbuild APO\SwapAPO\SwapAPO.vcxproj /p:Configuration=Debug /p:Platform=x64
```

build 成功後輸出在：

```bat
C:\Users\26100\Desktop\Windows-driver-samples\audio\sysvad\APO\SwapAPO\x64\Debug\SwapAPO.dll
C:\Users\26100\Desktop\Windows-driver-samples\audio\sysvad\APO\SwapAPO\x64\Debug\SwapAPO.pdb
```

## 部署

部署需要系統管理員權限。用系統管理員開 `cmd` 後執行：

```bat
net stop audiosrv /y
copy /Y "C:\Users\26100\Desktop\Windows-driver-samples\audio\sysvad\APO\SwapAPO\x64\Debug\SwapAPO.dll" "C:\Windows\System32\SwapAPO.dll"
copy /Y "C:\Users\26100\Desktop\Windows-driver-samples\audio\sysvad\APO\SwapAPO\x64\Debug\SwapAPO.pdb" "C:\Windows\System32\SwapAPO.pdb"
net start audiosrv
```

如果出現 `System error 5 Access is denied`，代表目前 cmd 不是系統管理員。

## Registry 開關

目前重點是 SFX：

```text
PKEY_Endpoint_Enable_Channel_Swap_SFX = {A44531EF-5377-4944-AE15-53789A9629C7},2
```

值是 `REG_DWORD`：

```text
1 = 開啟 SFX swap
0 = 關閉 SFX swap
```

如果只想測 SFX，請確認 MFX 的 swap 沒有同時開著，否則可能 swap 兩次，最後聽起來像沒有交換：

```text
PKEY_Endpoint_Enable_Channel_Swap_MFX = {A44531EF-5377-4944-AE15-53789A9629C7},3
```

建議：

```text
SFX ,2 = 1
MFX ,3 = 0
```

改完 registry 後，通常要重啟音訊服務或重新觸發 audio stream。

## 怎麼看 log

可以用 `AudioSessionMonitor.exe` 看 `OutputDebugString`：

```text
[APO] [QPC=...][Type=SFX][...]
```

注意事項：

- 如果看不到 `[APO]`，請用系統管理員執行 monitor。
- 不要同時 attach WinDbg 到 `audiodg.exe`，因為 WinDbg 可能會先吃掉 `OutputDebugString`。
- `[Type=Session]` 是 AudioSessionMonitor 印出的 session 事件。
- `[APO] [Type=SFX]` 是 `audiodg.exe` 裡的 APO 印出的 debug output。

## WAV dump 行為

目前的策略是避免產生一堆空檔：

- `LockForProcess()` 只記住 format，不會立刻開 WAV。
- 第一個 `BUFFER_VALID` 且不是全 0 的 buffer 進來時，才會建立 WAV。
- `BUFFER_SILENT` 不會 dump。
- 如果 stream 結束時沒有寫入任何 audio data，檔案會被刪掉。

所以關掉 YouTube 或沒有聲音時，即使 Windows 建立新的 SFX stream，也不應該留下空 WAV。

## Debug-only 注意事項

這個 WAV dump 是 debug 用，不適合長時間開著：

- `APOProcess()` 在 audio processing path 裡，寫檔會增加負擔。
- 長時間播放會產生很大的 WAV。
- 測完可以先清掉舊檔：

```bat
del C:\SwapApoDll\SwapAPOSFX_*.wav
```

如果要做正式版本，建議把 WAV dump 包在 debug flag 或 registry 開關後面，不要預設啟用。
