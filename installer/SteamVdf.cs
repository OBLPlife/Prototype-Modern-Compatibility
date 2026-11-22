using System;
using System.Collections.Generic;
using System.IO;
using System.Text;

namespace PrototypeModern {
    // Lossless edits: parse to locate one property, splice only that property's
    // text. Never serialize the rest of a Steam user's configuration.
    public sealed class SteamVdf {
        sealed class Node {
            public string name,value;
            public int start,valueStart,end,close;
            public List<Node> children;
        }
        readonly string text;
        int position;
        readonly Node root;
        public SteamVdf(string source) {text=source;root=new Node{children=Parse(false)};Skip();if(position!=text.Length)throw new IOException(InstallerText.VdfFormat);}
        void Skip() {
            while(position<text.Length) {
                if(Char.IsWhiteSpace(text[position])||text[position]=='\ufeff'){++position;continue;}
                if(position+1<text.Length&&text[position]=='/'&&text[position+1]=='/') {
                    while(position<text.Length&&text[position]!='\n')++position;continue;
                }
                break;
            }
        }
        string Token() {
            Skip();if(position>=text.Length||text[position++]!='"')throw new IOException(InstallerText.VdfSyntax);
            var value=new StringBuilder();
            while(position<text.Length) {
                char c=text[position++];if(c=='"')return value.ToString();
                if(c=='\\') {
                    if(position==text.Length)break;
                    char next=text[position++];
                    if(next=='n')value.Append('\n');else if(next=='r')value.Append('\r');else if(next=='t')value.Append('\t');
                    else if(next=='"'||next=='\\')value.Append(next);else value.Append('\\').Append(next);
                }else value.Append(c);
            }
            throw new IOException(InstallerText.VdfQuote);
        }
        List<Node> Parse(bool nested) {
            var entries=new List<Node>();
            while(true) {
                Skip();if(position==text.Length){if(nested)throw new IOException(InstallerText.VdfOpen);return entries;}
                if(text[position]=='}'){if(!nested)throw new IOException(InstallerText.VdfClose);return entries;}
                var entry=new Node{start=position,name=Token()};Skip();entry.valueStart=position;
                if(position<text.Length&&text[position]=='{'){++position;entry.children=Parse(true);entry.close=position;++position;}
                else entry.value=Token();
                entry.end=position;entries.Add(entry);
            }
        }
        Node Find(params string[] path) {
            Node current=root;
            foreach(string name in path) {
                Node match=null;if(current.children==null)return null;
                foreach(Node child in current.children)if(String.Equals(child.name,name,StringComparison.OrdinalIgnoreCase)) {
                    if(match!=null)throw new IOException(InstallerText.VdfDuplicate+name);match=child;
                }
                if(match==null)return null;current=match;
            }
            return current;
        }
        public string Get(params string[] path) {var entry=Find(path);return entry==null?null:entry.value;}
        public bool HasObject(params string[] path) {var entry=Find(path);return entry!=null&&entry.children!=null;}
        static readonly string[] AppPath={"UserLocalConfigStore","Software","Valve","Steam","apps","10150"};
        static readonly string[] OptionsPath={"UserLocalConfigStore","Software","Valve","Steam","apps","10150","LaunchOptions"};
        public bool HasPrototype {get{return HasObject(AppPath);}}
        public string Options {get{return Get(OptionsPath);}}
        static string Quote(string value) {return "\""+value.Replace("\\","\\\\").Replace("\"","\\\"").Replace("\r","\\r").Replace("\n","\\n").Replace("\t","\\t")+"\"";}
        int LineStart(int index) {int start=text.LastIndexOf('\n',Math.Max(0,index-1));return start<0?0:start+1;}
        bool OnlyIndent(int first,int last){for(int i=first;i<last;++i)if(text[i]!=' '&&text[i]!='\t')return false;return true;}
        public string SetOptions(string value) {
            Node app=Find(AppPath);if(app==null||app.children==null)throw new IOException(InstallerText.VdfAccountGame);
            Node entry=Find(OptionsPath);
            if(entry!=null) {
                if(entry.children!=null)throw new IOException(InstallerText.VdfLaunchType);
                if(value!=null)return text.Substring(0,entry.valueStart)+Quote(value)+text.Substring(entry.end);
                int first=LineStart(entry.start),last=entry.end;
                if(!OnlyIndent(first,entry.start))first=entry.start;
                else {
                    while(last<text.Length&&(text[last]==' '||text[last]=='\t'))++last;
                    if(last<text.Length&&text[last]=='\r')++last;
                    if(last<text.Length&&text[last]=='\n')++last;else last=entry.end;
                }
                return text.Remove(first,last-first);
            }
            if(value==null)return text;
            int lineStart=LineStart(app.close);string newline=text.Contains("\r\n")?"\r\n":"\n";
            if(OnlyIndent(lineStart,app.close)) {
                string indent=text.Substring(lineStart,app.close-lineStart)+"\t";
                return text.Insert(lineStart,indent+"\"LaunchOptions\"\t\t"+Quote(value)+newline);
            }
            return text.Insert(app.close,"\"LaunchOptions\" "+Quote(value)+" ");
        }
    }
}
