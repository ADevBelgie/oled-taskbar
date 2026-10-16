$ErrorActionPreference='Stop'
# Synthetic geometry fixtures: no real monitor configuration or device identity.
# Pure verification tests: never changes live desktop, registry, or startup.
$assembly=[Reflection.Assembly]::LoadFile((Join-Path $PSScriptRoot 'dist\OledTaskbar2.exe'))
$verify=$assembly.GetType('OledTaskbarV2.Panel').GetMethod('Verify',[Reflection.BindingFlags]'NonPublic,Static')
$enabled=@'
\\.\DISPLAY1 primary=1 bounds=0,0,1200,800 work=0,0,1200,800 display=PRIMARY
\\.\DISPLAY2 primary=0 bounds=1200,0,2100,640 work=1200,0,2100,600 display=SECONDARY
Shell_TrayWnd hwnd=1 pid=2 visible=1 rect=1200,600,2100,640
'@
$disabled=@'
\\.\DISPLAY1 primary=1 bounds=0,0,1200,800 work=0,0,1200,800 display=PRIMARY
\\.\DISPLAY2 primary=0 bounds=1200,0,2100,640 work=1200,0,2100,640 display=SECONDARY
Shell_TrayWnd hwnd=1 pid=2 visible=1 rect=0,760,1200,800
Shell_SecondaryTrayWnd hwnd=3 pid=2 visible=1 rect=1200,600,2100,640
'@
# Primary display switched off: only the secondary remains and becomes primary.
$single=@'
\\.\DISPLAY2 primary=1 bounds=0,0,900,640 work=0,0,900,640 display=SECONDARY
Shell_TrayWnd hwnd=1 pid=2 visible=1 rect=0,638,900,678
'@
$cases=@(
    @('Valid enabled',$enabled,$true,-1,$true),
    @('OLED reservation rejected',$enabled.Replace('work=0,0,1200,800','work=0,0,1200,400'),$true,-1,$false),
    @('Missing secondary display reservation rejected',$enabled.Replace('work=1200,0,2100,600','work=1200,0,2100,640'),$true,-1,$false),
    @('Missing primary rejected',$enabled.Replace('primary=1','primary=0'),$true,-1,$false),
    @('Hidden native taskbar rejected',$enabled.Replace('visible=1','visible=0'),$true,-1,$false),
    @('Secondary taskbar rejected',$enabled+"`nShell_SecondaryTrayWnd hwnd=3 visible=1 rect=0,760,1200,800",$true,-1,$false),
    @('Valid restore',$disabled,$false,1,$true),
    @('Corrupted restore work area rejected',$disabled.Replace('work=0,0,1200,800','work=0,0,1200,400'),$false,1,$false),
    @('Missing restored secondary rejected',($disabled -replace '(?m)^Shell_SecondaryTrayWnd.*',''),$false,1,$false),
    @('Unexpected restored secondary rejected',$disabled,$false,0,$false),
    @('Single-display restore accepted',$single,$false,1,$true),
    @('Single-display enable rejected',$single,$true,-1,$false),
    @('Resolution-independent geometry',$enabled.Replace('1200','1000').Replace('800','700').Replace('2100','1900'),$true,-1,$true)
)
foreach($case in $cases){
    $accepted=$true
    try{$verify.Invoke($null,@([string]$case[1],[bool]$case[2],[int]$case[3],$true))}catch{
        $failure=$_.Exception;while($failure.InnerException){$failure=$failure.InnerException}
        if($failure.GetType() -ne [System.Exception]){throw}
        $accepted=$false
    }
    if($accepted -ne $case[4]){throw "FAIL: $($case[0])"}
    Write-Output "PASS: $($case[0])"
}
# Symbol refresh after a Windows update: only a well-formed public identifier becomes a URL.
$symbolUrl=$assembly.GetType('OledTaskbarV2.Panel').GetMethod('SymbolUrl',[Reflection.BindingFlags]'NonPublic,Static')
$symbolCases=@(
    @('Symbol URL built','{"Pdb":"Taskbar.pdb","Key":"0123456789ABCDEF0123456789ABCDEF1","Timestamp":1,"Size":2}','https://msdl.microsoft.com/download/symbols/Taskbar.pdb/0123456789ABCDEF0123456789ABCDEF1/Taskbar.pdb'),
    @('Symbol path traversal rejected','{"Pdb":"..\\Taskbar.pdb","Key":"0123","Timestamp":1,"Size":2}',$null),
    @('Symbol key injection rejected','{"Pdb":"Taskbar.pdb","Key":"01/../x","Timestamp":1,"Size":2}',$null)
)
foreach($case in $symbolCases){
    $url=$null
    try{$url=$symbolUrl.Invoke($null,@([string]$case[1],$null))}catch{
        $failure=$_.Exception;while($failure.InnerException){$failure=$failure.InnerException}
        if($failure.GetType() -ne [System.Exception]){throw}
    }
    if($url -ne $case[2]){throw "FAIL: $($case[0])"}
    Write-Output "PASS: $($case[0])"
}
