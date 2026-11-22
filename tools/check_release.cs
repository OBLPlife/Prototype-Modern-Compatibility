// Exercise the shipped setup assemblies without running Steam or the game.
// Build as an x86 .NET Framework console application; see RELEASE.md.
using System;
using System.Collections.Generic;
using System.Diagnostics;
using System.Drawing;
using System.IO;
using System.Linq;
using System.Reflection;
using System.Security.Cryptography;
using System.Text;
using System.Web.Script.Serialization;
using System.Windows.Forms;

class CheckRelease {
    static readonly UTF8Encoding Utf8=new UTF8Encoding(false);
    static readonly JavaScriptSerializer Json=new JavaScriptSerializer();
    static int checks;
    static void Check(bool ok,string message) {
        if(!ok)throw new InvalidOperationException(message);
        checks++;Console.WriteLine("PASS "+message);
    }
    static string Hash(byte[] b) {
        using(var sha=SHA256.Create())return BitConverter.ToString(sha.ComputeHash(b)).Replace("-","").ToLowerInvariant();
    }
    static string FileHash(string p){return Hash(File.ReadAllBytes(p));}
    static byte[] Resource(Assembly app,string name) {
        using(var input=app.GetManifestResourceStream(name))using(var output=new MemoryStream()){
            input.CopyTo(output);return output.ToArray();
        }
    }
    static string Run(Assembly app,string root,string action) {
        Type t=app.GetType("PrototypeModern.Installer",true);
        try{return (string)t.GetMethod(action).Invoke(Activator.CreateInstance(t,root),null);}
        catch(TargetInvocationException e){throw e.InnerException??e;}
    }
    static void Interface(Assembly app,bool english) {
        using(var f=(Form)Activator.CreateInstance(app.GetType("PrototypeModern.MainWindow",true))) {
            var a=f.Controls.Find("InstallButton",false).Single() as Button;
            var b=f.Controls.Find("RestoreButton",false).Single() as Button;
            Check(f.Controls.Count==2&&a!=null&&b!=null&&a.Text==(english?"Install":"安装")&&b.Text==(english?"Restore":"还原"),
                "exactly two localized buttons "+app.GetName().Name);
            Check(f.Text==(english?"Prototype Modern Compatibility Patch":"虐杀原形现代化兼容补丁")&&
                  f.FormBorderStyle==FormBorderStyle.FixedDialog&&!f.MaximizeBox&&!f.MinimizeBox,
                  "localized title and fixed dialog");
            double scale=f.ClientSize.Width/380.0;
            var values=new[]{f.ClientSize.Height,a.Left,a.Top,a.Width,a.Height,b.Left,b.Top,b.Width,b.Height};
            var expected=new[]{84,60,22,120,40,200,22,120,40};
            Check(values.Select((v,i)=>Math.Abs(v-expected[i]*scale)<=2).All(x=>x),"Road Rash client size and button geometry");
        }
    }
    sealed class Library {
        public readonly string root,config,initialOptions,initialConfig;
        public Library(string dir,string engine,string launcher,bool customOptions) {
            root=Path.Combine(dir,"steamapps","common","Prototype");
            config=Path.Combine(dir,"userdata","42","config","localconfig.vdf");
            Directory.CreateDirectory(root);Directory.CreateDirectory(Path.GetDirectoryName(config));
            string steam=Path.Combine(dir,"steam.exe");
            File.WriteAllText(steam,"fixture data");
            string manifest="\"AppState\"{\"appid\" \"10150\" \"installdir\" \"Prototype\" \"LauncherPath\" \""+steam.Replace("\\","\\\\")+"\" \"LastOwner\" \"76561197960265770\"}";
            File.WriteAllText(Path.Combine(dir,"steamapps","appmanifest_10150.acf"),manifest,Utf8);
            initialOptions=customOptions?" -windowed -novid":"";
            initialConfig="\"UserLocalConfigStore\"\r\n{\r\n\"Unrelated\" \"KEEP\"\r\n\"Software\"{\"Valve\"{\"Steam\"{\"apps\"{\"10150\"{\r\n\"Playtime\" \"11\"\r\n"+
                (customOptions?"\"LaunchOptions\" \""+initialOptions.Trim()+"\"\r\n":"")+"}}}}}\r\n}\r\n";
            File.WriteAllText(config,initialConfig,Utf8);
            File.Copy(engine,Engine);File.Copy(launcher,Game);
            foreach(string n in new[]{"binkw32.dll","art.rcf","skuinfo.p3d"})File.WriteAllBytes(Path.Combine(root,n),new byte[0]);
            File.WriteAllText(Path.Combine(root,"version.dll"),"foreign proxy fixture",Utf8);
            File.WriteAllText(Path.Combine(root,"user-save.fixture"),"user save fixture",Utf8);
        }
        public string Engine{get{return Path.Combine(root,"prototypeenginef.dll");}}
        public string Game{get{return Path.Combine(root,"prototypef.exe");}}
        public string Module{get{return Path.Combine(root,"PrototypeModern.dll");}}
        public string Bridge{get{return Path.Combine(root,"PrototypeModern_Start.exe");}}
        public string Ini{get{return Path.Combine(root,"PrototypeModern.Performance.ini");}}
        public string Capture{get{return Path.Combine(root,"PrototypeModern.Capture");}}
        public string Journal{get{return Path.Combine(root,"PrototypeModern.Backup","manifest.json");}}
    }
    static void Cycle(Assembly install,Assembly restore,Library f,string original,string launcher,bool english,bool customCapture) {
        if(customCapture)File.WriteAllText(f.Ini,"[Capture]\r\nEnabled=0\r\n; Player-owned configuration\r\n",Encoding.Unicode);
        var external=customCapture?FileHash(f.Ini):null;
        string result=Run(install,f.root,"Install");
        Check(result==(english?"Installation complete. Start Prototype from Steam.":"安装完成。请从 Steam 启动游戏进行验证。"),"localized installation result");
        Check(FileHash(f.Engine)=="98521ac239852fd308446da3eacb6004083e490f3be0c39778ba99abd82701a2"&&FileHash(f.Game)==launcher&&FileHash(f.Module)==Hash(Resource(install,"module"))&&
            FileHash(f.Bridge)==Hash(Resource(install,"startupBridge")),"original launcher and exact installed payloads");
        var patched=FileHash(f.Engine);var config=FileHash(f.config);
        if(customCapture)Check(external==FileHash(f.Ini)&&!Directory.Exists(f.Capture),"external performance configuration preserved");
        else if(Environment.Is64BitOperatingSystem) {
            Check(File.ReadAllText(f.Ini).Contains(Path.Combine(f.Capture,"auto_capture.ps1"))&&File.Exists(Path.Combine(f.Capture,"LICENSE.txt")),
                "portable collector enabled with local bootstrap and runtime license");
            var start=new ProcessStartInfo(Path.Combine(f.Capture,"python.exe"),"-B -X utf8 capture_text.py --help") {
                WorkingDirectory=f.Capture,UseShellExecute=false,RedirectStandardOutput=true,RedirectStandardError=true,CreateNoWindow=true
            };
            using(var p=Process.Start(start)) {
                string output=p.StandardOutput.ReadToEnd();string error=p.StandardError.ReadToEnd();p.WaitForExit();
                Check(p.ExitCode==0&&output.Contains("--creation"),"bundled Python imports both collectors: "+error.Trim());
            }
        }
        Run(restore,f.root,"Install");
        Check(FileHash(f.Engine)==patched&&FileHash(f.config)==config,"reinstall across languages preserves the engine and Steam config");
        string before=File.ReadAllText(f.config);
        File.WriteAllText(f.config,before.Replace("KEEP","EDITED"),Utf8);
        string restored=Run(restore,f.root,"Restore");
        Check(restored==(english?"已还原启动程序与引擎，移除兼容模块。存档和个人设置保持不变。":"Restore complete. The original launcher and engine are in place. Save files and personal settings were preserved."),"other-language restore result");
        Run(install,f.root,"Restore");
        Check(FileHash(f.Engine)==original&&FileHash(f.Game)==launcher&&!File.Exists(f.Module)&&!File.Exists(f.Bridge),
            "cross-language and repeated restore return exact original game files");
        Check(File.ReadAllText(f.config)==f.initialConfig.Replace("KEEP","EDITED")&&
            File.ReadAllText(Path.Combine(f.root,"version.dll"))=="foreign proxy fixture"&&
            File.ReadAllText(Path.Combine(f.root,"user-save.fixture"))=="user save fixture","own launch option restored, other edits, proxy and saves preserved");
        Check(!Directory.Exists(f.Capture)&&(customCapture?external==FileHash(f.Ini):!File.Exists(f.Ini)),"owned collector removed and external configuration preserved");
    }
    static void Legacy(Assembly cn,Assembly en,Library f,string original,string legacyRecord) {
        File.WriteAllText(f.Ini,"[Capture]\r\nEnabled=0\r\n",Encoding.Unicode);
        Run(cn,f.root,"Install");
        byte[] old=File.ReadAllBytes(legacyRecord),bundle=File.ReadAllBytes(f.Module);
        File.WriteAllBytes(f.Journal,old);
        byte[] foreign=(byte[])bundle.Clone();foreign[foreign.Length-1]^=1;File.WriteAllBytes(f.Module,foreign);
        var beforeConfig=FileHash(f.config);bool refused=false;
        try{Run(en,f.root,"Restore");}catch(IOException e){refused=e.Message.Contains("PrototypeModern.dll");}
        Check(refused&&FileHash(f.Module)==Hash(foreign)&&FileHash(f.config)==beforeConfig&&FileHash(f.Journal)==Hash(old),"unknown DLL refused before touching other components");
        File.WriteAllBytes(f.Module,bundle);
        Run(en,f.root,"Install");
        var state=Json.Deserialize<Dictionary<string,object>>(File.ReadAllText(f.Journal));
        Check((string)state["moduleHash"]==Hash(bundle)&&state["previousModuleHash"]==null&&FileHash(f.config)==beforeConfig,"known field installation upgraded and stale ownership normalized");
        File.WriteAllBytes(f.Journal,old);
        Run(cn,f.root,"Restore");
        Check(FileHash(f.Engine)==original&&!File.Exists(f.Module)&&File.ReadAllText(f.config)==f.initialConfig,"direct restore from original field journal");
    }
    static object Review(Assembly app,string root,out object installer) {
        var type=app.GetType("PrototypeModern.Installer",true);
        installer=Activator.CreateInstance(type,root);
        return type.GetMethod("PrepareInstall").Invoke(installer,null);
    }
    static string Message(object review){return (string)review.GetType().GetProperty("WarningText").GetValue(review,null);}
    static void DifferentSources(Assembly cn,Assembly en,string work,string stock,string exe) {
        string root=Path.Combine(work,"standalone 游戏");
        Directory.CreateDirectory(root);
        string engine=Path.Combine(root,"prototypeenginef.dll"),launcher=Path.Combine(root,"prototypef.exe");
        string mod=Path.Combine(root,"PrototypeModern.dll"),link=Path.Combine(root,"PrototypeModern_Play.lnk");
        File.WriteAllText(Path.Combine(root,"PrototypeModern.Performance.ini"),"[Capture]\r\nEnabled=0\r\n",Encoding.Unicode);
        File.Copy(exe,launcher);
        byte[] original=File.ReadAllBytes(stock),variant=(byte[])original.Clone();
        // A PE build timestamp is independent of the engine resources touched by the patch.
        variant[BitConverter.ToInt32(variant,0x3c)+8]^=1;File.WriteAllBytes(engine,variant);
        object installer;
        Check(Message(Review(cn,root,out installer))=="","non-Steam installation with different whole-file hash and no unrelated assets has no warning");
        Run(cn,root,"Install");
        Check(File.Exists(link)&&File.ReadAllBytes(link)[0]==0x4c&&FileHash(engine)!=Hash(variant)&&FileHash(launcher)==FileHash(exe),
            "standalone installation patches the engine and creates a local Windows shortcut");
        Run(en,root,"Restore");
        Check(FileHash(engine)==Hash(variant)&&!File.Exists(link)&&!File.Exists(mod),"standalone cross-language restore preserves the actual variant");
        var pe=Activator.CreateInstance(cn.GetType("PrototypeModern.PeImage"),(object)original);
        int raw=(int)pe.GetType().GetMethod("Offset").Invoke(pe,new object[]{0x641580,80});
        variant[raw+64]^=1;File.WriteAllBytes(engine,variant);
        byte[] previousModule=Encoding.UTF8.GetBytes("pre-existing alternate module");File.WriteAllBytes(mod,previousModule);
        string before=FileHash(engine);
        object cnReview=Review(cn,root,out installer),unused;
        object enReview=Review(en,root,out unused);
        Check(Message(cnReview)=="所需资源存在变化，可能无法达到预期效果，是否继续安装？"&&
            Message(enReview)=="The required resources have changed and the patch may not work as expected. Continue installing?",
            "changed resources produce exactly the single localized question");
        Check(FileHash(engine)==before&&FileHash(mod)==Hash(previousModule)&&!File.Exists(link),
            "declining after preview leaves all target files unchanged");
        installer.GetType().GetMethod("InstallReviewed").Invoke(installer,new[]{cnReview});
        Check(FileHash(engine)!=before&&File.ReadAllBytes(engine)[raw+64]==variant[raw+64]&&FileHash(mod)==Hash(Resource(cn,"module")),
            "continuing applies the real bootstrap while preserving the variant resource body");
        Run(en,root,"Restore");
        Check(FileHash(engine)==before&&FileHash(mod)==Hash(previousModule),"restore recovers both the modified game file and the pre-existing module");
        raw=(int)pe.GetType().GetMethod("Offset").Invoke(pe,new object[]{0x85efb0,6});
        variant[raw]^=0x40;File.WriteAllBytes(engine,variant);
        string partial=Run(en,root,"Install");
        Check(partial.Contains("bootstrap")&&FileHash(engine)==Hash(variant)&&FileHash(mod)==Hash(Resource(en,"module")),
            "unusable engine entry still permits package installation without rewriting an unknown instruction");
        Run(cn,root,"Restore");
        Check(FileHash(engine)==Hash(variant)&&FileHash(mod)==Hash(previousModule),"partial installation restores the exact local inputs");
        var steam=new Library(Path.Combine(work,"modified-steam"),stock,exe,true);
        variant=(byte[])original.Clone();
        raw=(int)pe.GetType().GetMethod("Offset").Invoke(pe,new object[]{0xcc827,14});
        variant[raw+7]^=1;File.WriteAllBytes(steam.Engine,variant);
        Check(Message(Review(en,steam.root,out unused))==Message(enReview),"later vehicle resource change uses the same one-line warning");
        Run(cn,steam.root,"Install");
        Check(FileHash(steam.Engine)!=Hash(variant)&&File.ReadAllText(steam.config).Contains("PrototypeModern_Start.exe"),
            "changed-resource Steam installation can retain Steam library integration");
        Run(en,steam.root,"Restore");
        Check(FileHash(steam.Engine)==Hash(variant)&&File.ReadAllText(steam.config)==steam.initialConfig,
            "changed-resource Steam restore returns local engine and launch options exactly");
    }
    [STAThread] static int Main(string[] args) {
        try {
            if(args.Length<5||args.Length>6)throw new ArgumentException("CN setup, EN setup, original engine, original launcher, new test folder, [local development engine journal]");
            string work=Path.GetFullPath(args[4]);if(Directory.Exists(work)||File.Exists(work))throw new ArgumentException("Test folder must be new.");
            Directory.CreateDirectory(work);
            var cn=Assembly.LoadFile(Path.GetFullPath(args[0]));var en=Assembly.LoadFile(Path.GetFullPath(args[1]));
            Interface(cn,false);Interface(en,true);
            Check(cn.GetName().Version==new Version("0.3.3.0")&&en.GetName().Version==cn.GetName().Version,"matching 0.3.3 version metadata");
            string[] names=cn.GetManifestResourceNames();
            Check(names.Length==7&&names.OrderBy(x=>x).SequenceEqual(en.GetManifestResourceNames().OrderBy(x=>x))&&
                names.All(n=>Hash(Resource(cn,n))==Hash(Resource(en,n))),"all seven embedded payloads identical across languages");
            string original=FileHash(args[2]),launcher=FileHash(args[3]);
            Check(original=="d331be205bdde99897952ee88261ab7e63c5c65e36a4660e2a367886a3537071","stock Steam engine verified");
            string empty=Path.Combine(work,"invalid");Directory.CreateDirectory(empty);
            foreach(var app in new[]{cn,en}) {
                string msg="";try{Run(app,empty,"Install");}catch(IOException ex){msg=ex.Message;}
                Check(msg.Contains(app==en?"Missing file":"缺少"),"wrong-folder error in selected language");
            }
            Cycle(cn,en,new Library(Path.Combine(work,"CN"),args[2],args[3],true),original,launcher,false,false);
            Cycle(en,cn,new Library(Path.Combine(work,"EN"),args[2],args[3],false),original,launcher,true,true);
            DifferentSources(cn,en,work,args[2],args[3]);
            if(args.Length==6) {
                Check(Hash(Resource(cn,"module"))=="78a30d809d710a5c28f564011a7bf91f6eede4dc7c24612dc87428def2545c08"&&
                      Hash(Resource(cn,"startupBridge"))=="6618c21cb7e00dbd30f372428c9d9333257493dc3d071963a28b7b69f09a50bf",
                      "embedded native binaries are byte-identical to the field build");
                Legacy(cn,en,new Library(Path.Combine(work,"field"),args[2],args[3],false),original,args[5]);
            }
            Console.WriteLine("Passed "+checks+" checks. No game or Steam process launched.");
            return 0;
        }catch(Exception e){Console.Error.WriteLine(e);return 1;}
    }
}

