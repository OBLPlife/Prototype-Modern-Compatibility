using System;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Text;
using System.Threading;
using System.Threading.Tasks;
using System.Web.Script.Serialization;
using System.Windows.Forms;

[assembly:AssemblyTitle(PrototypeModern.Installer.ProductName)]
[assembly:AssemblyProduct(PrototypeModern.Installer.EnglishName)]
[assembly:AssemblyDescription(PrototypeModern.InstallerText.Description)]
[assembly:AssemblyVersion("0.3.3.0")]
[assembly:AssemblyFileVersion("0.3.3.0")]

namespace PrototypeModern {
    public class Journal {
        public int schema=1;
        public string version="0.1.0", state, originalHash, patchedHash, moduleHash, previousModuleHash, details;
    }
    public sealed class Installer {
        public const string ProductName=InstallerText.Title;
        public const string EnglishName="Prototype Modern Compatibility Patch";
        public const string EngineName="prototypeenginef.dll", ModuleName="PrototypeModern.dll";
        readonly string root, engine, module, backup, journalPath, originalPath;
        readonly JavaScriptSerializer json=new JavaScriptSerializer();
        readonly byte[] payload;
        readonly BuildProfile profile;
        readonly StartupInstaller startup;
        readonly EarlyInstaller early;
        readonly BridgeInstaller bridge;
        readonly CaptureInstaller capture;
        readonly ResourceInstaller resources;
        public string Version { get { return profile.version; } }
        public Installer(string directory) {
            root=Path.GetFullPath(directory); engine=Path.Combine(root,EngineName); module=Path.Combine(root,ModuleName);
            backup=Path.Combine(root,"PrototypeModern.Backup");
            journalPath=Path.Combine(backup,"manifest.json"); originalPath=Path.Combine(backup,"prototypeenginef.dll.original");
            payload=Resource("module"); profile=json.Deserialize<BuildProfile>(Encoding.UTF8.GetString(Resource("profile")));
            startup=new StartupInstaller(root,new byte[0]);
            early=new EarlyInstaller(root,new byte[0]);
            byte[] helper=Resource("startupBridge");
            bridge=new BridgeInstaller(root,helper);
            capture=new CaptureInstaller(root);
            resources=new ResourceInstaller(root,bridge,capture,payload,helper);
        }
        internal static byte[] Resource(string name) {
            using(var stream=Assembly.GetExecutingAssembly().GetManifestResourceStream(name)) {
                if(stream==null) throw new InvalidDataException(InstallerText.MissingResource+name);
                using(var memory=new MemoryStream()) { stream.CopyTo(memory); return memory.ToArray(); }
            }
        }
        internal static void RejectReparse(string path) {
            if((File.Exists(path)||Directory.Exists(path)) && (File.GetAttributes(path)&FileAttributes.ReparsePoint)!=0)
                throw new IOException(InstallerText.Reparse+path);
        }
        void CheckEnvironment() {
            foreach(string path in new[]{root,engine,module,backup,originalPath,journalPath}) RejectReparse(path);
            if(!File.Exists(engine)&&!File.Exists(Path.Combine(root,"prototypef.exe")))
                throw new IOException(InstallerText.GameLocation+EngineName+" / prototypef.exe");
            foreach(var process in Process.GetProcessesByName("prototypef")) {
                using(process) {
                    string image;
                    try {image=BridgeInstaller.ProcessPath(process);}catch {if(process.HasExited)continue;throw new IOException(InstallerText.GameProcessPath);}
                    if(String.Equals(Path.GetFullPath(image),Path.Combine(root,"prototypef.exe"),StringComparison.OrdinalIgnoreCase))
                        throw new IOException(InstallerText.CloseGame);
                }
            }
        }
        static string HashFile(string file) { return PatchImage.Hash(File.ReadAllBytes(file)); }
        internal static void AtomicWrite(string path,byte[] bytes) {
            string temporary=path+".tmp"; RejectReparse(path); RejectReparse(temporary);
            using(var stream=new FileStream(temporary,FileMode.Create,FileAccess.Write,FileShare.None)) {
                stream.Write(bytes,0,bytes.Length); stream.Flush(true);
            }
            if(File.Exists(path)) File.Replace(temporary,path,null); else File.Move(temporary,path);
        }
        void Save(Journal record) { AtomicWrite(journalPath,Encoding.UTF8.GetBytes(json.Serialize(record))); }
        Journal Read() {
            string readable=File.Exists(journalPath)?journalPath:journalPath+".tmp";
            if(!File.Exists(readable)) return null;
            RejectReparse(readable);
            Journal record=json.Deserialize<Journal>(File.ReadAllText(readable,Encoding.UTF8));
            if(record==null||record.schema!=1||record.version!="0.1.0"||
                !ValidHash(record.originalHash)||!ValidHash(record.patchedHash)||!ValidHash(record.moduleHash)||
                (record.previousModuleHash!=null&&(!ValidHash(record.previousModuleHash)||(record.state!="prepared"&&!FieldInstallation(record))))||
                (record.state!="prepared"&&record.state!="installed"&&record.state!="restored"))
                throw new InvalidDataException(InstallerText.JournalInvalid);
            return record;
        }
        // One development deployment updated the DLL directly after saving this journal.
        // Accept only its recorded fingerprint and the exact module we shipped from it.
        static bool FieldInstallation(Journal record) {
            return record.state=="installed" &&
                record.moduleHash=="a0d8447b10017c249f4b366b76568e4d57fa9d63cf9079f99278035ac2e07aae" &&
                record.previousModuleHash=="c69a92acafc0281994fd218b38692a4a7292205621c19b5647f5b5ed1671de6d";
        }
        static bool ValidHash(string hash) {
            if(hash==null||hash.Length!=64) return false;
            foreach(char c in hash) if(!((c>='0'&&c<='9')||(c>='a'&&c<='f'))) return false;
            return true;
        }
        void VerifyBackup(Journal record) {
            if(!File.Exists(originalPath)||HashFile(originalPath)!=record.originalHash)
                throw new IOException(InstallerText.MissingBackup);
        }
        void VerifyOwnedModule(Journal record) {
            if(File.Exists(module)) {
                string actual=HashFile(module);
                if(actual!=record.moduleHash&&(record.state!="prepared"||actual!=record.previousModuleHash)&&
                    !(FieldInstallation(record)&&actual=="78a30d809d710a5c28f564011a7bf91f6eede4dc7c24612dc87428def2545c08"))
                    throw new IOException(InstallerText.ForeignModule);
            }
        }
        IDisposable Lock() {
            string name="Local\\PrototypeModern-"+PatchImage.Hash(Encoding.UTF8.GetBytes(root.ToUpperInvariant()));
            var mutex=new Mutex(false,name);
            bool acquired;
            try { acquired=mutex.WaitOne(0); } catch(AbandonedMutexException) { acquired=true; }
            if(!acquired) {mutex.Dispose();throw new IOException(InstallerText.Busy);}
            return new OwnedMutex(mutex);
        }
        sealed class OwnedMutex:IDisposable {
            readonly Mutex mutex; public OwnedMutex(Mutex value){mutex=value;}
            public void Dispose(){mutex.ReleaseMutex();mutex.Dispose();}
        }
        bool RecordOperation(string action,string phase,string detail) {
            try {
                File.AppendAllText(Path.Combine(root,"PrototypeModern.Install.log"),
                    json.Serialize(new {time=DateTimeOffset.Now.ToString("o"),version=profile.version,
                        action=action,phase=phase,detail=detail})+Environment.NewLine,new UTF8Encoding(false));
                return true;
            } catch(IOException error) {Debug.WriteLine(error.Message);return false;}
              catch(UnauthorizedAccessException error) {Debug.WriteLine(error.Message);return false;}
        }
        string RunRecorded(string action,Func<string> operation) {
            bool recorded=RecordOperation(action,"begin","");
            try {
                string result=operation();
                recorded=RecordOperation(action,"completed",result)&&recorded;
                return recorded?result:result+InstallerText.LogFailed;
            } catch(Exception error) {RecordOperation(action,"failed",error.Message);throw;}
        }
        static bool ResourceError(Exception error) {
            return error is IOException||error is InvalidDataException||error is OverflowException||
                error is ArgumentException||error is InvalidOperationException||
                error is IndexOutOfRangeException||error is FormatException;
        }
        public InstallReview PrepareInstall() {
            using(Lock()) {
                CheckEnvironment();
                var plan=new InstallReview{Root=root,Capture=true};
                var file=new FileInfo(engine);
                plan.EngineLength=file.Exists?file.Length:-1;
                plan.EngineWriteTime=file.Exists?file.LastWriteTimeUtc.Ticks:0;
                byte[] current=file.Exists?File.ReadAllBytes(engine):null,baseline=current;
                plan.EngineInput=current;
                bool variant=resources.HasRecord;
                Journal record=null;
                try {
                    record=Read();
                    if(record!=null) {
                        VerifyBackup(record);
                        string localState=current==null?null:PatchImage.Hash(current);
                        if(localState==record.patchedHash)baseline=File.ReadAllBytes(originalPath);
                        else if(localState!=record.originalHash) {
                            variant=true;
                            if(current!=null)baseline=PatchImage.BeforeOwnBootstrap(File.ReadAllBytes(originalPath),current,profile)??current;
                        }
                        VerifyOwnedModule(record);
                    } else if((File.Exists(module)&&!resources.Owns(ModuleName))||
                              (!variant&&Directory.Exists(backup)&&Directory.GetFileSystemEntries(backup).Length!=0))
                        plan.NeedsWarning=true;
                } catch(Exception error){if(!ResourceError(error))throw;plan.NeedsWarning=true;}
                if(variant) {
                    byte[] prior=resources.EngineBaseline(profile);if(prior!=null)baseline=prior;
                    if(resources.HasChanges())plan.NeedsWarning=true;
                }
                string launcherPath=Path.Combine(root,"prototypef.exe");
                bool launcher=File.Exists(launcherPath);
                if(launcher) {
                    try{if(!LauncherPatch.HasStartupReceive(File.ReadAllBytes(launcherPath)))plan.NeedsWarning=true;}
                    catch(Exception error){if(!ResourceError(error))throw;plan.NeedsWarning=true;}
                }
                plan.Steam=launcher&&bridge.IsSteamLocation()&&!(variant&&resources.HasShortcut);
                if(plan.Steam) {
                    try {bridge.CheckInstall();}
                    catch(Exception error){
                        if(!ResourceError(error))throw;
                        plan.Steam=false;
                        if(error.Message!=InstallerText.CloseSteam&&error.Message!=InstallerText.SteamRunningUnknown)plan.NeedsWarning=true;
                    }
                }
                else if(!launcher)plan.NeedsWarning=true;
                try{capture.CheckInstall();}catch(Exception error){if(!ResourceError(error))throw;plan.Capture=false;plan.NeedsWarning=true;}
                try{startup.CheckRestore();early.CheckRestore();}catch(Exception error){if(!ResourceError(error))throw;plan.NeedsWarning=true;}
                if(baseline!=null) {
                    try {
                        var pe=new PeImage(baseline);
                        if(pe.Sections.Any(s=>s.Name==".pmc")) {
                            plan.NeedsWarning=true;
                        } else {
                            if(PatchImage.ResourcesDiffer(baseline,profile))plan.NeedsWarning=true;
                            string details;
                            plan.EngineOutput=PatchImage.CreateVariant(baseline,profile,out details);
                            plan.HasEntry=true;
                        }
                    } catch(Exception error){if(!ResourceError(error))throw;plan.NeedsWarning=true;}
                } else plan.NeedsWarning=true;
                plan.Helper=launcher&&plan.HasEntry;
                if(!plan.HasEntry)plan.Steam=false;
                if(!plan.Steam&&plan.Helper) {
                    foreach(string name in new[]{BridgeInstaller.ModuleName,ResourceInstaller.Shortcut}) {
                        if(File.Exists(Path.Combine(root,name))&&!resources.Owns(name))plan.NeedsWarning=true;
                    }
                }
                plan.Variant=variant||!plan.Steam||plan.NeedsWarning;
                return plan;
            }
        }
        public string Install() {return InstallReviewed(PrepareInstall());}
        public string InstallReviewed(InstallReview plan) {
            if(plan==null||plan.Root!=root)throw new ArgumentException(InstallerText.ReviewAgain);
            return RunRecorded("install",delegate {
                using(Lock()) {
                    var now=PrepareInstall();
                    if(plan.EngineLength!=now.EngineLength||plan.EngineWriteTime!=now.EngineWriteTime||
                        plan.Variant!=now.Variant||plan.Steam!=now.Steam||
                        plan.NeedsWarning!=now.NeedsWarning||plan.Capture!=now.Capture||plan.HasEntry!=now.HasEntry)
                        throw new IOException(InstallerText.ReviewAgain);
                    return now.Variant?resources.Install(now):InstallCore();
                }
            });
        }
        public string Restore() {return RunRecorded("restore",RestoreCore);}
        string InstallCore() {
            using(Lock()) {
                CheckEnvironment();startup.CheckRestore();early.CheckRestore();
                capture.CheckInstall();
                startup.Restore();bridge.CheckInstall();early.Restore();
                Journal previous=Read();bool engineAlreadyInstalled=previous!=null&&HashFile(engine)==previous.patchedHash;
                byte[] previousModule=engineAlreadyInstalled&&File.Exists(module)?File.ReadAllBytes(module):null;
                InstallEngine();
                bool addedCapture=false;
                try {addedCapture=capture.Install();bridge.Install();}
                catch(Exception error) {
                    string recovery;
                    try {
                        if(addedCapture)capture.Restore();
                        if(engineAlreadyInstalled) {
                            if(previousModule!=null)AtomicWrite(module,previousModule);else if(File.Exists(module))File.Delete(module);
                            Save(previous);recovery=InstallerText.PreviousRestored;
                        } else {bridge.Restore();recovery=RestoreEngine();}
                    }
                    catch(Exception rollback){recovery=InstallerText.RollbackIncomplete+rollback.Message;}
                    throw new IOException(error.Message+Environment.NewLine+recovery,error);
                }
                return InstallerText.Installed;
            }
        }
        string InstallEngine() {
            using(Lock()) {
                CheckEnvironment();
                byte[] current=File.ReadAllBytes(engine); string currentHash=PatchImage.Hash(current);
                Journal record=Read();
                if(record!=null) {
                    VerifyOwnedModule(record);
                    if(record.moduleHash!=PatchImage.Hash(payload)) {
                        VerifyBackup(record);string upgradeDetails;
                        byte[] checkedImage=PatchImage.Create(File.ReadAllBytes(originalPath),profile,out upgradeDetails);
                        if(PatchImage.Hash(checkedImage)!=record.patchedHash||
                            (currentHash!=record.patchedHash&&currentHash!=record.originalHash))
                            throw new IOException(InstallerText.LayoutChanged);
                        record.previousModuleHash=File.Exists(module)?HashFile(module):record.moduleHash;record.moduleHash=PatchImage.Hash(payload);
                        record.details=upgradeDetails;record.state="prepared";Save(record);
                    }
                    if(currentHash==record.patchedHash) {
                        VerifyBackup(record);
                        if(!File.Exists(module)||HashFile(module)!=record.moduleHash) AtomicWrite(module,payload);
                        record.previousModuleHash=null;record.state="installed";Save(record);
                        return InstallerText.AlreadyInstalled;
                    }
                    if(currentHash!=record.originalHash)
                        throw new IOException(InstallerText.ForeignEngine);
                } else {
                    if(File.Exists(module)) throw new IOException(InstallerText.UnownedModule);
                    if(Directory.Exists(backup)&&Directory.GetFileSystemEntries(backup).Length!=0)
                        throw new IOException(InstallerText.UnownedBackup);
                }
                string details;
                byte[] patched=PatchImage.Create(current,profile,out details);
                // Compatibility uses independent function fingerprints,
                // an export relationship and actual import slots, not full-file hash alone.
                if(record==null) {
                    record=new Journal {state="prepared",originalHash=currentHash,
                        patchedHash=PatchImage.Hash(patched),moduleHash=PatchImage.Hash(payload),details=details};
                    Directory.CreateDirectory(backup); Save(record);
                } else {
                    if(record.patchedHash!=PatchImage.Hash(patched)) throw new IOException(InstallerText.RebuildMismatch);
                    record.state="prepared"; Save(record);
                }
                try {
                    if(!File.Exists(originalPath)) AtomicWrite(originalPath,current);
                    VerifyBackup(record);
                    string staged=Path.Combine(backup,"engine.staged"); AtomicWrite(staged,patched);
                    if(HashFile(staged)!=record.patchedHash) throw new IOException(InstallerText.StagingFailed);
                    if(!File.Exists(module)||HashFile(module)!=record.moduleHash) AtomicWrite(module,payload);
                    VerifyOwnedModule(record);
                    CheckEnvironment();
                    if(HashFile(engine)!=record.originalHash) throw new IOException(InstallerText.EngineWriteConflict);
                    File.Replace(staged,engine,null);
                    if(HashFile(engine)!=record.patchedHash) throw new IOException(InstallerText.InstallVerifyFailed);
                    record.previousModuleHash=null;record.state="installed"; Save(record);
                    return InstallerText.EngineInstalled;
                } catch(Exception failure) {
                    string rollback="";
                    try {
                        VerifyBackup(record); VerifyOwnedModule(record);
                        string actual=HashFile(engine);
                        if(actual==record.patchedHash) AtomicWrite(engine,File.ReadAllBytes(originalPath));
                        else if(actual!=record.originalHash) throw new IOException(InstallerText.EngineRollbackConflict);
                        if(File.Exists(module)) File.Delete(module);
                        record.previousModuleHash=null;record.state="restored"; Save(record);
                        rollback=InstallerText.EngineRestored;
                    } catch(Exception recovery) { rollback=InstallerText.RecordKept+recovery.Message+InstallerText.Retry; }
                    throw new IOException(failure.Message+Environment.NewLine+rollback,failure);
                }
            }
        }
        string RestoreCore() {
            using(Lock()) {
                CheckEnvironment();
                if(resources.HasRecord)return resources.Restore();
                startup.CheckRestore();early.CheckRestore();bridge.CheckRestore();capture.CheckRestore();
                Journal record=Read();
                if(record!=null) {
                    VerifyBackup(record);VerifyOwnedModule(record);
                    string hash=HashFile(engine);
                    if(hash!=record.originalHash&&hash!=record.patchedHash)
                        throw new IOException(InstallerText.RestoreConflict);
                }
                bridge.Restore();early.Restore();startup.Restore();RestoreEngine();capture.Restore();
                return InstallerText.Restored;
            }
        }
        string RestoreEngine() {
            using(Lock()) {
                CheckEnvironment(); Journal record=Read();
                if(record==null) return InstallerText.NoRecord;
                VerifyBackup(record); VerifyOwnedModule(record);
                string current=HashFile(engine);
                if(current!=record.originalHash&&current!=record.patchedHash)
                    throw new IOException(InstallerText.EngineOtherPatch);
                if(current==record.patchedHash) AtomicWrite(engine,File.ReadAllBytes(originalPath));
                if(HashFile(engine)!=record.originalHash) throw new IOException(InstallerText.RestoreVerifyFailed);
                if(File.Exists(module)) File.Delete(module);
                record.previousModuleHash=null;record.state="restored"; Save(record);
                return InstallerText.OriginalRestored;
            }
        }
        public string Inspect(string input,string stagedOutput) {
            if(String.Equals(Path.GetExtension(input),".exe",StringComparison.OrdinalIgnoreCase)||
                String.Equals(Path.GetFileName(input),"prototypef.exe.original",StringComparison.OrdinalIgnoreCase))
                return bridge.Inspect(input,stagedOutput);
            string details; byte[] original=File.ReadAllBytes(input);
            byte[] staged=PatchImage.Create(original,profile,out details);
            var pe=new PeImage(staged);
            if(stagedOutput!=null) {
                if(String.Equals(Path.GetFullPath(input),Path.GetFullPath(stagedOutput),StringComparison.OrdinalIgnoreCase))
                    throw new IOException(InstallerText.InspectInput);
                AtomicWrite(stagedOutput,staged);
            }
            return json.Serialize(new {version=profile.version,mode="static-only",gameLaunched=false,
                originalSha256=PatchImage.Hash(original),patchedSha256=PatchImage.Hash(staged),
                moduleSha256=PatchImage.Hash(payload), originalSize=original.Length,patchedSize=staged.Length,
                sectionCount=pe.Count,details=details,runtimeVerification="not performed; user owns game testing"});
        }
    }

    public sealed class MainWindow:Form {
        readonly Button install=new Button(),restore=new Button();
        readonly Installer installer;
        public MainWindow() {
            installer=new Installer(AppDomain.CurrentDomain.BaseDirectory);
            Text=Installer.ProductName;
            Font=new Font("Microsoft YaHei UI",10F);
            AutoScaleDimensions=new SizeF(96F,96F); AutoScaleMode=AutoScaleMode.Dpi;
            ClientSize=new Size(380,84); FormBorderStyle=FormBorderStyle.FixedDialog;
            MaximizeBox=false; MinimizeBox=false; StartPosition=FormStartPosition.CenterScreen;
            install.Name="InstallButton"; install.Text=InstallerText.Install; install.SetBounds(60,22,120,40);
            restore.Name="RestoreButton"; restore.Text=InstallerText.Restore; restore.SetBounds(200,22,120,40);
            install.Click+=async (s,e)=>await Run(false); restore.Click+=async (s,e)=>await Run(true);
            Controls.AddRange(new Control[]{install,restore});
        }
        async Task Run(bool restoreMode) {
            install.Enabled=restore.Enabled=false; UseWaitCursor=true;
            try {
                string result;
                if(restoreMode)result=await Task.Run(()=>installer.Restore());
                else {
                    var plan=await Task.Run(()=>installer.PrepareInstall());
                    if(plan.NeedsWarning&&MessageBox.Show(this,plan.WarningText,InstallerText.ResourceTitle,
                        MessageBoxButtons.YesNo,MessageBoxIcon.Warning,MessageBoxDefaultButton.Button2)!=DialogResult.Yes)return;
                    result=await Task.Run(()=>installer.InstallReviewed(plan));
                }
                MessageBox.Show(this,result,Text,MessageBoxButtons.OK,MessageBoxIcon.Information);
            } catch(Exception error) {
                MessageBox.Show(this,error.Message,Text,MessageBoxButtons.OK,MessageBoxIcon.Error);
            } finally { install.Enabled=restore.Enabled=true; UseWaitCursor=false; }
        }
        protected override void OnFormClosing(FormClosingEventArgs e) {
            if(!install.Enabled) {e.Cancel=true;return;} base.OnFormClosing(e);
        }
    }
    static class Program {
        [STAThread] static int Main(string[] args) {
            System.Globalization.CultureInfo.DefaultThreadCurrentUICulture =
                System.Globalization.CultureInfo.GetCultureInfo(InstallerText.Locale);
            // Offline authoring/checking interface. No command starts the game.
            if(args.Length>=1&&args[0]=="--inspect") {
                if(args.Length<3||args.Length>4) return 2;
                string inputPath=Path.GetFullPath(args[1]);
                string reportPath=Path.GetFullPath(args[2]);
                if(!String.Equals(Path.GetExtension(reportPath),".json",StringComparison.OrdinalIgnoreCase)||
                    String.Equals(inputPath,reportPath,StringComparison.OrdinalIgnoreCase)) return 2;
                if(args.Length==4 && (String.Equals(Path.GetFullPath(args[3]),inputPath,StringComparison.OrdinalIgnoreCase)||
                    String.Equals(Path.GetFullPath(args[3]),reportPath,StringComparison.OrdinalIgnoreCase))) return 2;
                try {
                    var installer=new Installer(Path.GetDirectoryName(inputPath));
                    string report=installer.Inspect(inputPath,args.Length==4?args[3]:null);
                    File.WriteAllText(reportPath,report,Encoding.UTF8);return 0;
                } catch(Exception error) {File.WriteAllText(reportPath,error.ToString(),Encoding.UTF8);return 1;}
            }
            if(args.Length!=0) return 2;
            Application.EnableVisualStyles(); Application.SetCompatibleTextRenderingDefault(false);
            Application.Run(new MainWindow()); return 0;
        }
    }
}
