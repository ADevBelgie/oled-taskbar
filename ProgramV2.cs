using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Runtime.InteropServices;
using System.Threading;
using System.Threading.Tasks;
using System.IO.Pipes;
using System.Windows.Forms;
using Microsoft.Win32;

namespace OledTaskbarV2
{
    internal static class Program
    {
        internal static readonly string Folder=AppDomain.CurrentDomain.BaseDirectory;
        internal const string Key=@"Software\OledTaskbar2";
        internal static readonly string PipeName="OledTaskbar2."+System.Security.Principal.WindowsIdentity.GetCurrent().User.Value+"."+Process.GetCurrentProcess().SessionId;
        internal static readonly string Data=Path.Combine(Environment.GetFolderPath(Environment.SpecialFolder.LocalApplicationData),"OledTaskbar2");
        internal static void Log(string text){Directory.CreateDirectory(Data);File.AppendAllText(Path.Combine(Data,"activity.log"),DateTime.Now.ToString("s")+" "+text+Environment.NewLine);}
        [STAThread] static void Main(string[] args)
        {
            string command=args.Length==0?"show":args[0].TrimStart('-');
            bool first;
            using(var mutex=new Mutex(true,@"Local\OledTaskbar2.Controller",out first))
            {
                if(!first)
                {
                    try {
                        using(var pipe=new NamedPipeClientStream(".",PipeName,PipeDirection.InOut,PipeOptions.Asynchronous))
                        {pipe.Connect(10000);var writer=new StreamWriter(pipe){AutoFlush=true};var reader=new StreamReader(pipe);writer.WriteLine(command);var reply=reader.ReadLineAsync();if(!reply.Wait(60000))throw new TimeoutException("Controller reply timed out.");int code;if(!int.TryParse(reply.Result,out code))throw new IOException("Invalid controller reply.");Environment.ExitCode=code;}
                    }
                    catch(Exception ex){Log(ex.Message);Environment.ExitCode=4;}
                    return;
                }
                Application.EnableVisualStyles();Application.SetCompatibleTextRenderingDefault(false);
                if(command=="disable"||command=="restore")
                {
                    using(var panel=new Panel(command))
                    {try{Log(panel.RestoreForCommand());Environment.ExitCode=0;}catch(Exception ex){Log(ex.ToString());Environment.ExitCode=1;}}
                    return;
                }
                Application.Run(new Panel(command));
            }
        }
    }
    internal sealed class Panel:Form
    {
        readonly Label status=new Label();readonly Button enable=new Button(),disable=new Button();
        readonly CheckBox startup=new CheckBox();readonly System.Windows.Forms.Timer debounce=new System.Windows.Forms.Timer();
        readonly System.Collections.Generic.List<EventWaitHandle> signals=new System.Collections.Generic.List<EventWaitHandle>();
        readonly System.Collections.Generic.List<RegisteredWaitHandle> waits=new System.Collections.Generic.List<RegisteredWaitHandle>();
        readonly uint taskbarCreated=RegisterWindowMessage("TaskbarCreated");
        bool busy,closing,loading=true,pendingReconcile;string initial;
        NamedPipeServerStream pipeServer; volatile bool pipeClosing;
        public Panel(string command)
        {
            initial=command; Text="OLED Taskbar"; ClientSize=new Size(560,295);MinimumSize=new Size(576,334);MaximizeBox=false;
            Icon=Icon.ExtractAssociatedIcon(Application.ExecutablePath);
            Controls.Add(new Label{Text="OLED Taskbar",Font=new Font("Segoe UI",16,FontStyle.Bold),Location=new Point(20,16),AutoSize=true});
            Controls.Add(new Label{Text="Keep the primary display clear. Full Windows taskbar on the secondary.",Location=new Point(20,55),Size=new Size(520,36)});
            status.SetBounds(20,97,520,72);status.BorderStyle=BorderStyle.FixedSingle;status.Padding=new Padding(8);status.Text="Checking configuration…";Controls.Add(status);
            enable.Text="Enable";enable.SetBounds(20,185,100,32);enable.Click+=async(s,e)=>await Change(true);Controls.Add(enable);
            disable.Text="Disable / restore";disable.SetBounds(130,185,130,32);disable.Click+=async(s,e)=>await Change(false);Controls.Add(disable);
            startup.Text="Start with Windows";startup.SetBounds(286,191,180,24);startup.Checked=StartupExists();startup.CheckedChanged+=(s,e)=>{if(!loading)SetStartup(startup.Checked);};Controls.Add(startup);
            var uninstall=new Button{Text="Uninstall…"};uninstall.SetBounds(20,239,100,32);uninstall.Click+=async(s,e)=>{if(await Change(false)){SetStartup(false);Process.Start(new ProcessStartInfo("powershell.exe","-NoProfile -ExecutionPolicy Bypass -File \""+Path.Combine(Program.Folder,"uninstall-v2.ps1")+"\""){UseShellExecute=false,CreateNoWindow=true});closing=true;Close();}};Controls.Add(uninstall);
            var log=new Button{Text="Diagnostic log"};log.SetBounds(130,239,130,32);log.Click+=(s,e)=>{Program.Log("Log opened");Process.Start("notepad.exe","\""+Path.Combine(Program.Data,"activity.log")+"\"");};Controls.Add(log);
            var exit=new Button{Text="Exit and restore"};exit.SetBounds(380,239,160,32);exit.Click+=async(s,e)=>{if(await Change(false)){closing=true;Close();}};Controls.Add(exit);
            debounce.Interval=1500;debounce.Tick+=async(s,e)=>{debounce.Stop();await Reconcile();};
            Shown+=async(s,e)=>{
                foreach(var screen in Screen.AllScreens)if(!screen.Primary){Location=new Point(screen.WorkingArea.Left+80,screen.WorkingArea.Top+90);break;}
                Register("show",()=>{Show();WindowState=FormWindowState.Normal;Activate();});
                Register("enable",async()=>await Change(true));Register("disable",async()=>await Change(false));Register("restore",async()=>await Change(false));
                SystemEvents.DisplaySettingsChanged+=DisplayChanged;SystemEvents.SessionSwitch+=SessionChanged;
                StartPipe();
                loading=false;
                if(initial=="enable")await Change(true);
                else if(initial=="disable"||initial=="restore"){bool restored=await Change(false);Environment.ExitCode=restored?0:1;closing=true;Close();return;}
                else if(WantsEnabled()||State()!="Disabled")await Reconcile();
                else status.Text="Disabled. Your original Windows taskbar behavior is active.";
                if(initial=="background"&&(State()=="Enabled"||(State()=="Disabled"&&!WantsEnabled())))Hide();
            };
            FormClosing+=async(s,e)=>{if(closing)return;if(e.CloseReason==CloseReason.WindowsShutDown)return;e.Cancel=true;Hide();await Task.CompletedTask;};
        }
        void StartPipe()
        {
            Task.Run(async()=>{
                while(!pipeClosing)
                {
                    try
                    {
                        var security=new PipeSecurity();security.SetAccessRuleProtection(true,false);
                        security.AddAccessRule(new PipeAccessRule(System.Security.Principal.WindowsIdentity.GetCurrent().User,PipeAccessRights.FullControl,System.Security.AccessControl.AccessControlType.Allow));
                        using(var server=new NamedPipeServerStream(Program.PipeName,PipeDirection.InOut,1,PipeTransmissionMode.Byte,PipeOptions.Asynchronous,4096,4096,security))
                        {
                            pipeServer=server;await server.WaitForConnectionAsync();
                            var reader=new StreamReader(server);var writer=new StreamWriter(server){AutoFlush=true};
                            var read=reader.ReadLineAsync();if(await Task.WhenAny(read,Task.Delay(10000))!=read)continue;
                            string command=await read;var done=new TaskCompletionSource<int>();
                            BeginInvoke((Action)(async()=>{
                                try{if(command=="show"){Show();WindowState=FormWindowState.Normal;Activate();done.SetResult(0);}
                                else if(command!="enable"&&command!="disable"&&command!="restore")done.SetResult(2);
                                else if(busy)done.SetResult(3);
                                else done.SetResult(await Change(command=="enable")?0:1);}
                                catch(Exception ex){Program.Log(ex.ToString());done.TrySetResult(1);}
                            }));
                            await writer.WriteLineAsync((await done.Task).ToString());
                        }
                    }
                    catch(Exception ex){if(!pipeClosing){Program.Log("Command channel: "+ex.Message);Thread.Sleep(1000);}}
                }
            });
        }
        string State(){using(var k=Registry.CurrentUser.CreateSubKey(Program.Key))return (string)k.GetValue("State","Disabled");}
        bool WantsEnabled(){using(var k=Registry.CurrentUser.CreateSubKey(Program.Key))return (int)k.GetValue("WantEnabled",State()=="Enabled"?1:0)!=0;}
        void SetWanted(bool value){using(var k=Registry.CurrentUser.CreateSubKey(Program.Key))k.SetValue("WantEnabled",value?1:0);}
        async Task Reconcile(){string state=State();if(state!="Disabled"&&state!="Enabled"&&!await Change(false,true))return;if(WantsEnabled())await Change(true,true);}
        internal string RestoreForCommand(){SetWanted(false);return Operate(false);}
        void WriteState(string value){using(var k=Registry.CurrentUser.CreateSubKey(Program.Key))k.SetValue("State",value);}
        void Register(string command,Action action){var ev=new EventWaitHandle(false,EventResetMode.AutoReset,@"Local\OledTaskbar2."+command);signals.Add(ev);waits.Add(ThreadPool.RegisterWaitForSingleObject(ev,(s,t)=>{try{BeginInvoke(action);}catch(InvalidOperationException){}},null,Timeout.Infinite,false));}
        async Task<bool> Change(bool desired,bool reconcile=false)
        {
            if(busy)return false;busy=true;enable.Enabled=disable.Enabled=false;status.Text=desired?"Applying taskbar placement…":"Restoring Windows…";
            try
            {
                if(!reconcile)SetWanted(desired);
                // Check before Operate changes auto-hide or the shell: after a Windows update
                // the bridge rejects stale symbols and Explorer cannot be configured.
                if(desired&&!await Task.Run(()=>SymbolsMatch()))
                {
                    Show();
                    if(reconcile||!ConfirmSymbolDownload())throw new Exception(StaleSymbols);
                    status.Text="Downloading matching Windows taskbar symbols from Microsoft…";
                    await Task.Run(()=>RefreshSymbols());
                    status.Text="Applying taskbar placement…";
                }
                string result=await Task.Run(()=>Operate(desired));status.Text=result;Program.Log(result);return true;
            }
            catch(Exception ex){status.Text="Needs attention: "+ex.Message;Program.Log(ex.ToString());Show();return false;}
            finally{busy=false;enable.Enabled=disable.Enabled=true;if(pendingReconcile){pendingReconcile=false;Schedule();}}
        }
        int RunNativeCode(string args,out string text)
        {
            using(var p=Process.Start(new ProcessStartInfo(Path.Combine(Program.Folder,"NativeControl.exe"),args){UseShellExecute=false,CreateNoWindow=true,RedirectStandardOutput=true,RedirectStandardError=true}))
            {
                var output=p.StandardOutput.ReadToEndAsync();var error=p.StandardError.ReadToEndAsync();
                if(!p.WaitForExit(45000)){p.Kill();p.WaitForExit();throw new Exception("Native operation timed out; helper stopped and recovery data retained.");}
                Task.WaitAll(output,error);Program.Log(args+" exit="+p.ExitCode+" "+output.Result+error.Result);
                text=p.ExitCode==0?output.Result:output.Result+error.Result;return p.ExitCode;
            }
        }
        string RunNative(string args)
        {
            string text;int code=RunNativeCode(args,out text);
            if(code!=0)throw new Exception("Shell operation failed ("+code+"). "+text);
            return text;
        }
        const string StaleSymbols="Windows updated its taskbar, so the saved taskbar symbols no longer match. Press Enable to download matching public symbols from Microsoft.";
        bool SymbolsMatch()
        {
            string text;int code=RunNativeCode("symbols-status",out text);
            if(code==0)return true;if(code==6)return false;
            throw new Exception("The installed Windows taskbar could not be identified ("+code+"). "+text);
        }
        bool ConfirmSymbolDownload()
        {
            return MessageBox.Show(this,"Windows updated its taskbar (taskbar.dll). OLED Taskbar needs the matching public debug symbols from Microsoft's symbol server before it can move the taskbar again.\n\nOnly the Windows module's public symbol identifier is sent. Download them now?",
                "OLED Taskbar",MessageBoxButtons.YesNo,MessageBoxIcon.Question)==DialogResult.Yes;
        }
        void RefreshSymbols()
        {
            string pdb;string url=SymbolUrl(RunNative("pdb-info"),out pdb);
            string folder=Path.Combine(Program.Data,"symbols");Directory.CreateDirectory(folder);
            string destination=Path.Combine(folder,pdb),partial=destination+".download";
            System.Net.ServicePointManager.SecurityProtocol|=System.Net.SecurityProtocolType.Tls12;
            try{using(var web=new System.Net.WebClient())web.DownloadFile(url,partial);}
            catch(System.Net.WebException ex){throw new Exception("Matching taskbar symbols could not be downloaded: "+ex.Message);}
            if(File.Exists(destination))File.Delete(destination);File.Move(partial,destination);
            // resolve writes bridge.ini next to NativeControl.exe; the bridge rereads it on the next Enable.
            string text;if(RunNativeCode("resolve \""+folder+"\"",out text)!=0||!SymbolsMatch())
                throw new Exception("Matching taskbar symbols were not found for this Windows build. OLED Taskbar stays disabled. "+text);
            Program.Log("Taskbar symbols refreshed for the updated Windows taskbar.");
        }
        static string SymbolUrl(string pdbInfo,out string pdb)
        {
            var match=System.Text.RegularExpressions.Regex.Match(pdbInfo,"\"Pdb\":\"([a-zA-Z0-9_-]+\\.pdb)\",\"Key\":\"([0-9A-F]+)\"");
            if(!match.Success)throw new Exception("Unexpected symbol identifier.");
            pdb=match.Groups[1].Value;
            return "https://msdl.microsoft.com/download/symbols/"+pdb+"/"+match.Groups[2].Value+"/"+pdb;
        }
        string Operate(bool desired)
        {
            string before=State();int owner=Process.GetCurrentProcess().Id;
            if(desired)
            {
                if(before!="Disabled"&&before!="Enabled"){Restore(owner);before="Disabled";}
                if(before=="Disabled")
                {
                    var bar=FindWindow("Shell_TrayWnd",null);if(bar==IntPtr.Zero)throw new Exception("Windows taskbar is not ready.");
                    using(var key=Registry.CurrentUser.CreateSubKey(Program.Key))
                    {key.SetValue("OriginalState",(int)GetState());key.SetValue("OriginalMM",ReadMM());key.SetValue("State","Enabling");}
                }
                try
                {
                    // Release the old monitor's reservation before moving the native bar.
                    // Reserve space only after Explorer has placed it on the secondary display.
                    SetState(1);
                    int originalAuto;using(var k=Registry.CurrentUser.OpenSubKey(Program.Key))originalAuto=(int)k.GetValue("OriginalState",1)&1;
                    RunNative("configure 1 "+owner+" "+originalAuto);
                    SetState(0);
                    VerifyEventually(true,-1);WriteState("Enabled");return "Enabled: full taskbar on the secondary display; primary display uses its full desktop area.";
                }
                catch {try{Restore(owner);}catch(Exception recovery){Program.Log("Rollback: "+recovery);WriteState("RecoveryNeeded");}throw;}
            }
            if(before=="Disabled"){VerifyEventually(false,ReadMM());return "Disabled. Original Windows taskbar behavior is active.";}
            Restore(owner);return "Disabled. Original Windows taskbar settings restored.";
        }
        void Restore(int owner)
        {
            WriteState("Restoring");
            using(var key=Registry.CurrentUser.CreateSubKey(Program.Key))
            {
                int original=(int)key.GetValue("OriginalState",-1),mm=(int)key.GetValue("OriginalMM",-2);
                if(original<0||mm==-2)throw new Exception("Saved original settings are missing.");
                Exception nativeError=null;
                try{RunNative("configure 0 "+owner+" "+(original&1));}catch(Exception ex){nativeError=ex;}
                SetState(original);
                if(nativeError!=null)throw nativeError;
                VerifyEventually(false,ReadMM(),(original&1)!=0);
                if(GetState()!=(uint)(original&1))throw new Exception("Auto-hide restoration could not be verified.");
            }
            WriteState("Disabled");
        }
        void VerifyEventually(bool desired,int mm,bool? autoHide=null)
        {
            Exception last=null;
            for(int attempt=0;attempt<10;attempt++)
            {try{Verify(RunNative("probe"),desired,mm,autoHide??(GetState()!=0));return;}catch(Exception ex){last=ex;}Thread.Sleep(300);}
            throw last;
        }
        static Rectangle Rect(string line,string field)
        {
            var match=System.Text.RegularExpressions.Regex.Match(line,field+@"=(-?\d+),(-?\d+),(-?\d+),(-?\d+)");
            if(!match.Success)throw new Exception("Incomplete monitor diagnostics.");
            return Rectangle.FromLTRB(int.Parse(match.Groups[1].Value),int.Parse(match.Groups[2].Value),int.Parse(match.Groups[3].Value),int.Parse(match.Groups[4].Value));
        }
        static void Verify(string text,bool desired,int originalMM,bool autoHide)
        {
            // Geometry is read by a separate unhooked process, avoiding the shell's modified monitor view.
            var lines=text.Split('\n');
            string primary=lines.FirstOrDefault(l=>l.Contains("primary=1"));
            if(primary==null||(desired&&lines.Count(l=>l.Contains("display="))!=2))throw new Exception("Expected primary display could not be verified.");
            var oledBounds=Rect(primary,"bounds");
            if(desired&&Rect(primary,"work")!=oledBounds)throw new Exception("The OLED's full desktop area was not preserved.");
            string secondary=lines.FirstOrDefault(l=>l.Contains("display=")&&l.Contains("primary=0"));
            string bar=lines.FirstOrDefault(l=>l.StartsWith("Shell_TrayWnd "));
            if(bar==null||!bar.Contains("visible=1"))throw new Exception("The real taskbar is not visible.");
            var barBounds=Rect(bar,"rect");
            if(desired)
            {
                if(secondary==null)throw new Exception("secondary display is not connected.");
                var bounds=Rect(secondary,"bounds");var work=Rect(secondary,"work");
                if(barBounds.Left!=bounds.Left||barBounds.Right!=bounds.Right||barBounds.Bottom!=bounds.Bottom||barBounds.Height<=0||barBounds.Height>bounds.Height/4)throw new Exception("The full taskbar is not correctly placed on the secondary display.");
                if(work!=Rectangle.FromLTRB(bounds.Left,bounds.Top,bounds.Right,barBounds.Top))throw new Exception("secondary display taskbar space was not reserved correctly.");
                if(lines.Any(l=>l.StartsWith("Shell_SecondaryTrayWnd")&&l.Contains("visible=1")))throw new Exception("An unwanted secondary taskbar remains visible.");
            }
            else
            {
                if(!oledBounds.IntersectsWith(barBounds))throw new Exception("The taskbar did not return to the OLED.");
                int actual=lines.Count(l=>l.StartsWith("Shell_SecondaryTrayWnd")&&l.Contains("visible=1"));
                int expected=originalMM==0?0:lines.Count(l=>l.Contains("display="))-1;
                if(actual!=expected)throw new Exception("Windows has not restored the original multi-monitor taskbar preference.");
                foreach(string monitor in lines.Where(l=>l.Contains("display=")))
                {
                    var bounds=Rect(monitor,"bounds");var work=Rect(monitor,"work");
                    var taskbar=lines.FirstOrDefault(l=>l.StartsWith("Shell_")&&l.Contains("visible=1")&&bounds.IntersectsWith(Rect(l,"rect")));
                    var expectedWork=autoHide||taskbar==null?bounds:Rectangle.FromLTRB(bounds.Left,bounds.Top,bounds.Right,Rect(taskbar,"rect").Top);
                    if(work!=expectedWork)throw new Exception("Windows has not restored a monitor's full usable desktop area.");
                }
            }
        }
        static int ReadMM(){using(var k=Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Explorer\Advanced"))return (int)(k.GetValue("MMTaskbarEnabled",-1));}
        static uint GetState(){var d=new APPBARDATA();d.cbSize=Marshal.SizeOf(d);return (uint)SHAppBarMessage(4,ref d).ToUInt64()&1;}
        static void SetState(int value){var d=new APPBARDATA();d.cbSize=Marshal.SizeOf(d);d.hWnd=FindWindow("Shell_TrayWnd",null);d.lParam=new IntPtr(value&1);SHAppBarMessage(10,ref d);if(GetState()!=(uint)(value&1))throw new Exception("Windows did not apply auto-hide preference.");}
        static bool StartupExists(){using(var k=Registry.CurrentUser.OpenSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run"))return k!=null&&k.GetValue("OledTaskbar2")!=null;}
        static void SetStartup(bool on){using(var k=Registry.CurrentUser.CreateSubKey(@"Software\Microsoft\Windows\CurrentVersion\Run")){if(on)k.SetValue("OledTaskbar2","\""+Application.ExecutablePath+"\" --background");else k.DeleteValue("OledTaskbar2",false);}}
        void Schedule(){if(busy){pendingReconcile=true;return;}if(WantsEnabled()||State()=="RecoveryNeeded"){debounce.Stop();debounce.Start();}}
        void DisplayChanged(object s,EventArgs e){try{BeginInvoke((Action)Schedule);}catch(InvalidOperationException){}}
        void SessionChanged(object s,SessionSwitchEventArgs e){if(e.Reason==SessionSwitchReason.SessionUnlock)DisplayChanged(s,e);}
        protected override void WndProc(ref Message m){if((uint)m.Msg==taskbarCreated&&!loading)Schedule();base.WndProc(ref m);}
        protected override void Dispose(bool disposing){if(disposing){pipeClosing=true;if(pipeServer!=null)pipeServer.Dispose();debounce.Dispose();SystemEvents.DisplaySettingsChanged-=DisplayChanged;SystemEvents.SessionSwitch-=SessionChanged;foreach(var w in waits)w.Unregister(null);foreach(var e in signals)e.Dispose();}base.Dispose(disposing);}
        [StructLayout(LayoutKind.Sequential)] struct APPBARDATA{public int cbSize;public IntPtr hWnd;public uint callback,edge;public int left,top,right,bottom;public IntPtr lParam;}
        [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern IntPtr FindWindow(string cls,string name);
        [DllImport("user32.dll",CharSet=CharSet.Unicode)]static extern uint RegisterWindowMessage(string value);
        [DllImport("shell32.dll")]static extern UIntPtr SHAppBarMessage(uint message,ref APPBARDATA data);
    }
}
