#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <map>
#include <set>
#include <dirent.h>
#include <sys/stat.h>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
#include <fcntl.h>
#include <signal.h>
#ifdef __APPLE__
#include <util.h>
#else
#include <pty.h>
#endif
#include <dlfcn.h>
#include <cstdint>
#include <cerrno>
using namespace std;

enum {
  K_UP=1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL,
  K_PGUP, K_PGDN, K_STAB, K_CLICK, K_WHEEL_UP, K_WHEEL_DOWN,
  K_PASTE_BEGIN, K_PASTE_END, K_WORD_LEFT, K_WORD_RIGHT,
  K_TAB=9, K_BACK=127
};
enum Mode { NORM, INS, CMD };

struct termios orig;
vector<string> L;
int cx=0, cy=0, off=0, coff=0, rows=24, cols=80, dirty=0;
int tabstop=4, expandtab=0, esc_ms=8, scrolloff=2, mouse=1;
int click_sx=1, click_sy=1, click_mod=0;
string fname, msg, cbuf, pend, terminal_shell;
vector<string> shell_popup;
Mode mode=NORM;
struct MultiCursor { int y=0,x=0; };
vector<MultiCursor> cursors;
bool split_vertical=false, split_horizontal=false;
int active_pane=0;
bool picker_open=false;
bool palette_open=false;
bool terminal_popup=false;
int terminal_fd=-1;
pid_t terminal_pid=-1;
vector<string> terminal_lines;
int terminal_cursor_x=0, terminal_cursor_y=0;
int terminal_parser_state=0; // 0=normal, 1=ESC, 2=CSI, 3=OSC
string terminal_csi_params, terminal_osc;
bool tree_sitter_enabled=true;

struct TSNode {
  uint32_t context[4];
  const void *id;
  const void *tree;
};
struct TSRuntime {
  void *runtime=nullptr, *grammar=nullptr;
  void *parser=nullptr, *tree=nullptr;
  const void *language_ptr=nullptr;
  TSNode (*root_node)(const void*)=nullptr;
  uint32_t (*child_count)(TSNode)=nullptr;
  TSNode (*child)(TSNode,uint32_t)=nullptr;
  const char* (*type)(TSNode)=nullptr;
  uint32_t (*start_byte)(TSNode)=nullptr;
  uint32_t (*end_byte)(TSNode)=nullptr;
  bool (*is_named)(TSNode)=nullptr;
  void* (*parser_new)()=nullptr;
  void (*parser_delete)(void*)=nullptr;
  bool (*parser_set_language)(void*,const void*)=nullptr;
  void* (*parser_parse_string)(void*,const void*,const char*,uint32_t)=nullptr;
  void (*tree_delete)(void*)=nullptr;
  string language, reason;
  vector<unsigned char> faces;
  vector<size_t> line_starts;
  uint64_t hash=0;
  bool attempted=false;
  bool ready=false;
} ts;
string picker_query;
vector<string> picker_files;
int picker_selected=0;
struct CustomCommand { string shell, cwd; };
map<string,CustomCommand> custom_commands;
struct PaneState {
  vector<string> lines; string name; int x=0,y=0,top=0,left=0,changed=0;
};
vector<PaneState> panes;
void reset_cursors();
void sync_primary();
void normalize_cursors();
string ext_of();
void load_custom_commands(const string&);
PaneState capture_pane(){
  return {L,fname,cx,cy,off,coff,dirty};
}
void restore_pane(const PaneState& p){
  L=p.lines; fname=p.name; cx=p.x; cy=p.y; off=p.top; coff=p.left; dirty=p.changed;
  if(L.empty()) L.push_back("");
  cy=max(0,min(cy,(int)L.size()-1)); cx=max(0,min(cx,(int)L[cy].size()));
}
void save_active_pane(){
  if(!panes.empty()) panes[active_pane]=capture_pane();
}
void load_active_pane(){
  if(!panes.empty()) restore_pane(panes[active_pane]);
}
struct SavedBuffer {
vector<string> lines;
string name;
int x=0, y=0, top=0, left=0, changed=0;
};
SavedBuffer saved_buffer;
bool scratch_buffer=false;

// The palette is intentionally close to the green terminal reference:
// quiet text on a deep desaturated green, with the chrome only slightly
// darker than the editor surface.
string BG       = "\x1b[48;2;31;43;36m";
string BG_BAR   = "\x1b[48;2;25;36;29m";
string FG       = "\x1b[38;2;151;164;151m";
string FG_DIM   = "\x1b[38;2;111;128;113m";
string FG_FAINT = "\x1b[38;2;76;101;84m";
string FG_BRIGHT= "\x1b[38;2;206;216;204m";
string POPUP_BG="\x1b[48;2;41;67;47m";
string POPUP_TEXT="\x1b[38;2;173;194;174m";
string POPUP_SELECTED_BG="\x1b[48;2;59;97;68m";
string POPUP_SELECTED_TEXT="\x1b[38;2;220;231;216m";
string POPUP_FACE=POPUP_BG+POPUP_TEXT;
string POPUP_SELECTED_FACE=POPUP_SELECTED_BG+POPUP_SELECTED_TEXT;
string SYN_COMMENT="\x1b[38;2;106;153;85m";
string SYN_STRING="\x1b[38;2;214;157;133m";
string SYN_NUMBER="\x1b[38;2;174;129;255m";
string SYN_KEYWORD="\x1b[38;2;255;198;109m";
static const char* RESET    = "\x1b[0m";

struct Snip { string trig, body; int only_bol=0, autoexp=0; };
vector<Snip> snips;
struct Stop { int id, y, x, len; };
struct Sess { vector<Stop> st; int idx=-1, sel=0; } sess;

struct CHelp { const char* name; const char* alias; const char* usage; const char* doc; const char* extra; };
static const CHelp CHELP[] = {
  {"q","quit","q","quit if the buffer is saved","refuses when [+] dirty — use :q!"},
  {"q!","quit!","q!","force quit, discard changes","does not write the file"},
  {"w","write","w [file]","write the buffer to disk","with a name, sets the filename then saves"},
  {"wq","x","wq","write the file, then quit","alias: :x"},
  {"x","wq","x","write the file, then quit","same as :wq"},
  {"e","edit","e <file>","open a file in this buffer","unsaved changes stay unless you :w first"},
  {"edit","e","edit <file>","open a file in this buffer","alias of :e"},
  {"snip","snippets","snip","reload UltiSnips snippet files","cwd, ~/.config/nv, vim/nvim UltiSnips"},
  {"snippets","snip","snippets","reload UltiSnips snippet files","alias of :snip"},
{"snippets-ls","snips","snippets-ls","list snippets for the current filetype","opens a read-only scratch buffer"},
  {"number","line","number <line>","jump to a line number","you can also type :42"},
  {"end","$","end","jump to the end of the file","same destination as normal-mode G"},
  {"sh","shell","sh <command>","run a shell command","stdout and stderr open in a small popup"},
  {"term","terminal","term","open an interactive shell popup","Escape closes the shell"},
  {"help","h","help [cmd]","list commands, or explain one","command completion appears as you type"},
  {"split","sp","split [file]","open a horizontal split","Ctrl-W then h/j/k/l focuses panes"},
  {"vsplit","vs","vsplit [file]","open a vertical split","shares the current buffer when no file is given"},
  {"close","only","close","close the active split","the last pane cannot be closed"},
  {"picker","find","picker","open the fuzzy file picker","Ctrl-P is the shortcut"},
  {"ts-status","ts","ts-status","show Tree-sitter runtime and grammar status","falls back to builtin highlighting when unavailable"},
};
static const int NHELP=(int)(sizeof(CHELP)/sizeof(CHELP[0]));

void die(const char* s){ perror(s); exit(1); }
void raw_off(){
  tcsetattr(0, TCSAFLUSH, &orig);
  const char* s="\x1b[?1000l\x1b[?1006l\x1b[?2004l\x1b[?7h\x1b[?25h\x1b[2 q\x1b[0m\x1b[?1049l";
  (void)!write(1,s,strlen(s));
}
void raw_on(){
  if(tcgetattr(0,&orig)==-1) die("tcgetattr");
  atexit(raw_off);
  termios r=orig;
  r.c_iflag &= ~(BRKINT|ICRNL|INPCK|ISTRIP|IXON);
  r.c_oflag &= ~(OPOST);
  r.c_cflag |= CS8;
  r.c_lflag &= ~(ECHO|ICANON|IEXTEN|ISIG);
  r.c_cc[VMIN]=1; r.c_cc[VTIME]=0;
  if(tcsetattr(0,TCSAFLUSH,&r)==-1) die("tcsetattr");
  const char* s="\x1b[?1049h\x1b[?2004h\x1b[2J\x1b[H";
  (void)!write(1,s,strlen(s));
}
void mouse_on(){
  if(!mouse) return;
  const char* s="\x1b[?1000h\x1b[?1006h";
  (void)!write(1,s,strlen(s));
}
void winsz(){
  winsize ws;
  if(ioctl(1,TIOCGWINSZ,&ws)==-1||!ws.ws_col){ rows=24; cols=80; }
  else { rows=ws.ws_row; cols=ws.ws_col; }
}

int jnum(const string& t, const char* k, int def){
  string key=string("\"")+k+"\"";
  auto p=t.find(key); if(p==string::npos) return def;
  p=t.find(':',p); if(p==string::npos) return def;
  return atoi(t.c_str()+p+1);
}
int jbool(const string& t, const char* k, int def){
  string key=string("\"")+k+"\"";
  auto p=t.find(key); if(p==string::npos) return def;
  p=t.find(':',p); if(p==string::npos) return def;
  size_t i=p+1; while(i<t.size() && isspace((unsigned char)t[i])) i++;
  if(t.compare(i,4,"true")==0) return 1;
  if(t.compare(i,5,"false")==0) return 0;
  return atoi(t.c_str()+i)?1:0;
}
string jstr(const string& t, const char* k, const string& def){
  string key=string("\"")+k+"\"";
  auto p=t.find(key); if(p==string::npos) return def;
  p=t.find(':',p); if(p==string::npos) return def;
  p=t.find('"',p+1); if(p==string::npos) return def;
  string out;
  for(size_t i=p+1;i<t.size();i++){
    if(t[i]=='"') return out;
    if(t[i]=='\\' && i+1<t.size()){
      char ch=t[++i];
      if(ch=='n') out+='\n';
      else if(ch=='t') out+='\t';
      else out+=ch;
    } else out+=t[i];
  }
  return def;
}
string rgb_face(const string& hex, bool background){
  if(hex.size()!=7 || hex[0]!='#') return "";
  for(size_t i=1;i<hex.size();i++)
    if(!isxdigit((unsigned char)hex[i])) return "";
  int r=(int)strtol(hex.substr(1,2).c_str(),0,16);
  int g=(int)strtol(hex.substr(3,2).c_str(),0,16);
  int b=(int)strtol(hex.substr(5,2).c_str(),0,16);
  return "\x1b[" + string(background?"48":"38") + ";2;" +
         to_string(r)+";"+to_string(g)+";"+to_string(b)+"m";
}
void apply_face(string& target, const string& t, const char* key, bool background){
  string value=jstr(t,key,"");
  if(value.empty()) return;
  string face=rgb_face(value,background);
  if(!face.empty()) target=face;
}
void apply_cfg(const string& t){
  tabstop=max(1,min(16,jnum(t,"tabstop",tabstop)));
  expandtab=jbool(t,"expandtab",expandtab);
  esc_ms=max(0,min(80,jnum(t,"esc_ms",esc_ms)));
  scrolloff=max(0,min(20,jnum(t,"scrolloff",scrolloff)));
  mouse=jbool(t,"mouse",mouse);
  tree_sitter_enabled=jbool(t,"tree_sitter",tree_sitter_enabled);
  terminal_shell=jstr(t,"terminal_shell",terminal_shell);
  apply_face(BG,t,"editor_bg",true);
  apply_face(BG_BAR,t,"status_bg",true);
  apply_face(FG,t,"text",false);
  apply_face(FG_DIM,t,"muted",false);
  apply_face(FG_FAINT,t,"faint",false);
  apply_face(FG_BRIGHT,t,"bright",false);
  apply_face(POPUP_BG,t,"popup_bg",true);
  apply_face(POPUP_TEXT,t,"popup_text",false);
  apply_face(POPUP_SELECTED_BG,t,"popup_selected_bg",true);
  apply_face(POPUP_SELECTED_TEXT,t,"popup_selected_text",false);
  apply_face(SYN_COMMENT,t,"syntax_comment",false);
  apply_face(SYN_STRING,t,"syntax_string",false);
  apply_face(SYN_NUMBER,t,"syntax_number",false);
  apply_face(SYN_KEYWORD,t,"syntax_keyword",false);
  POPUP_FACE=POPUP_BG+POPUP_TEXT;
  POPUP_SELECTED_FACE=POPUP_SELECTED_BG+POPUP_SELECTED_TEXT;
}
string fallback_syntax_face(const string& s,int i){
  string e=ext_of();
  bool code=e=="c"||e=="h"||e=="cpp"||e=="cc"||e=="js"||e=="ts"||e=="json"||e=="py"||e=="sh"||e=="bash";
  if(!code && e!="md" && e!="markdown") return FG;
  size_t slash=s.find("//"), hash=s.find('#');
  if((slash!=string::npos&&(size_t)i>=slash)||(hash!=string::npos&&(size_t)i>=hash)) return SYN_COMMENT;
  bool quote=false; for(int j=0;j<i;j++) if(s[j]=='"'&& (j==0||s[j-1]!='\\')) quote=!quote;
  if(quote||s[i]=='"') return SYN_STRING;
  if(isdigit((unsigned char)s[i])) return SYN_NUMBER;
  if(isalpha((unsigned char)s[i])||s[i]=='_'){
    int a=i; while(a>0&&(isalnum((unsigned char)s[a-1])||s[a-1]=='_')) a--;
    int b=i; while(b<(int)s.size()&&(isalnum((unsigned char)s[b])||s[b]=='_')) b++;
    string w=s.substr(a,b-a);
    static const set<string> kw={"if","else","for","while","return","class","struct","def","import","from","include","int","char","void","const","let","var","function","true","false","null","async","await","echo","case","in"};
    if(kw.count(w)) return SYN_KEYWORD;
  }
  return FG;
}

string ts_language_for_ext(){
  string e=ext_of();
  if(e=="c"||e=="h") return "c";
  if(e=="cc"||e=="cpp"||e=="cxx"||e=="hpp") return "cpp";
  if(e=="js"||e=="jsx") return "javascript";
  if(e=="ts") return "typescript";
  if(e=="tsx") return "tsx";
  if(e=="py") return "python";
  if(e=="sh"||e=="bash"||e=="zsh") return "bash";
  if(e=="json") return "json";
  if(e=="md"||e=="markdown") return "markdown";
  return "";
}
template<class T> bool ts_sym(void *h,const char *name,T& out){
  out=reinterpret_cast<T>(dlsym(h,name));
  return out!=nullptr;
}
uint64_t ts_fingerprint(){
  uint64_t h=1469598103934665603ULL;
  auto add=[&](unsigned char c){ h^=c; h*=1099511628211ULL; };
  for(const string& s:L){ for(unsigned char c:s) add(c); add('\n'); }
  for(unsigned char c:fname) add(c);
  return h;
}
string ts_join_buffer(){
  string out;
  for(size_t i=0;i<L.size();i++){
    if(i) out+='\n';
    out+=L[i];
  }
  return out;
}
string ts_find_grammar(const string& lang){
  vector<string> dirs={".rune/parsers"};
  if(const char* h=getenv("HOME")){
    dirs.push_back(string(h)+"/.config/rune/parsers");
    dirs.push_back(string(h)+"/.local/lib");
  }
  dirs.push_back("/usr/lib"); dirs.push_back("/usr/local/lib");
  dirs.push_back("/lib");
  vector<string> names={lang+".so","libtree-sitter-"+lang+".so",
                        "tree-sitter-"+lang+".so"};
  for(const string& d:dirs) for(const string& n:names){
    string p=d+"/"+n;
    if(access(p.c_str(),R_OK)==0) return p;
  }
  return "";
}
void ts_close_tree(){
  if(ts.tree && ts.tree_delete) ts.tree_delete(ts.tree);
  ts.tree=nullptr;
}
void ts_cleanup(){
  ts_close_tree();
  if(ts.parser && ts.parser_delete) ts.parser_delete(ts.parser);
  ts.parser=nullptr;
  if(ts.grammar) dlclose(ts.grammar);
  if(ts.runtime) dlclose(ts.runtime);
  ts.grammar=ts.runtime=nullptr; ts.language_ptr=nullptr;
  ts.ready=false;
}
bool ts_load(const string& lang){
  if(!tree_sitter_enabled){ ts.reason="disabled by configuration"; return false; }
  if(ts.ready && ts.language==lang){
    return true;
  }
  ts_cleanup();
  ts.reason.clear();
  if(lang.empty()){ ts.reason="no grammar mapping for this extension"; return false; }
  const char* runtimes[]={"libtree-sitter.so","libtree-sitter.so.0",
                          "libtree-sitter.so.1",nullptr};
  for(int i=0;runtimes[i]&&!ts.runtime;i++) ts.runtime=dlopen(runtimes[i],RTLD_NOW|RTLD_LOCAL);
  if(!ts.runtime){ ts.reason="Tree-sitter runtime not found"; return false; }
  bool ok=true;
  ok&=ts_sym(ts.runtime,"ts_parser_new",ts.parser_new);
  ok&=ts_sym(ts.runtime,"ts_parser_delete",ts.parser_delete);
  ok&=ts_sym(ts.runtime,"ts_parser_set_language",ts.parser_set_language);
  ok&=ts_sym(ts.runtime,"ts_parser_parse_string",ts.parser_parse_string);
  ok&=ts_sym(ts.runtime,"ts_tree_delete",ts.tree_delete);
  ok&=ts_sym(ts.runtime,"ts_tree_root_node",ts.root_node);
  ok&=ts_sym(ts.runtime,"ts_node_child_count",ts.child_count);
  ok&=ts_sym(ts.runtime,"ts_node_child",ts.child);
  ok&=ts_sym(ts.runtime,"ts_node_type",ts.type);
  ok&=ts_sym(ts.runtime,"ts_node_start_byte",ts.start_byte);
  ok&=ts_sym(ts.runtime,"ts_node_end_byte",ts.end_byte);
  ok&=ts_sym(ts.runtime,"ts_node_is_named",ts.is_named);
  if(!ok){ ts.reason="Tree-sitter runtime is missing required symbols"; ts_cleanup(); return false; }
  string path=ts_find_grammar(lang);
  if(path.empty()){ ts.reason="grammar '"+lang+"' not found"; ts_cleanup(); return false; }
  ts.grammar=dlopen(path.c_str(),RTLD_NOW|RTLD_LOCAL);
  if(!ts.grammar){ ts.reason="cannot load grammar '"+lang+"'"; ts_cleanup(); return false; }
  string symbol="tree_sitter_"+lang;
  using GrammarFn=const void* (*)();
  GrammarFn fn=reinterpret_cast<GrammarFn>(dlsym(ts.grammar,symbol.c_str()));
  if(!fn){ ts.reason="grammar has no "+symbol+" symbol"; ts_cleanup(); return false; }
  ts.language_ptr=fn();
  ts.parser=ts.parser_new();
  if(!ts.parser || !ts.language_ptr ||
     !ts.parser_set_language(ts.parser,ts.language_ptr)){
    ts.reason="grammar rejected by Tree-sitter"; ts_cleanup(); return false;
  }
  ts.language=lang; // Keep the selected name; parser owns the opaque language.
  ts.ready=true;
  return true;
}
unsigned char ts_classify(const string& type, bool named){
  string t=type; for(char& c:t) c=(char)tolower((unsigned char)c);
  if(t.find("comment")!=string::npos) return 1;
  if(t.find("string")!=string::npos||t.find("character")!=string::npos) return 2;
  if(t.find("number")!=string::npos||t.find("integer")!=string::npos||
     t.find("float")!=string::npos) return 3;
  if(t.find("type")!=string::npos) return 4;
  if(!named && !t.empty() &&
     all_of(t.begin(),t.end(),[](char c){return isalpha((unsigned char)c)||c=='_';}))
    return 4;
  return 0;
}
void ts_mark(TSNode n){
  if(!ts.type||!ts.start_byte||!ts.end_byte) return;
  string type=ts.type(n)?ts.type(n):"";
  unsigned char cls=ts_classify(type,ts.is_named?ts.is_named(n):true);
  uint32_t a=ts.start_byte(n), b=ts.end_byte(n);
  if(cls) for(uint32_t i=a;i<b&&i<ts.faces.size();i++) ts.faces[i]=cls;
  if(ts.child_count&&ts.child){
    uint32_t count=ts.child_count(n);
    for(uint32_t i=0;i<count;i++) ts_mark(ts.child(n,i));
  }
}
void ts_prepare(){
  uint64_t h=ts_fingerprint();
  string lang=ts_language_for_ext();
  if(!tree_sitter_enabled){
    ts.ready=false; ts.language=lang; ts.hash=h; ts.faces.clear(); ts.reason="disabled by configuration"; return;
  }
  if(ts.hash==h && ts.language==lang && ts.attempted) return;
  ts.hash=h; ts.language=lang; ts.attempted=true;
  ts.faces.clear(); ts.line_starts.clear();
  if(!ts_load(lang)) return;
  string text=ts_join_buffer();
  ts.faces.assign(text.size(),0);
  for(size_t i=0;i<L.size();i++){
    size_t start=i?ts.line_starts.back()+L[i-1].size()+1:0;
    ts.line_starts.push_back(start);
  }
  ts_close_tree();
  ts.tree=ts.parser_parse_string(ts.parser,nullptr,text.data(),(uint32_t)text.size());
  if(!ts.tree){ ts.reason="parser returned no tree"; return; }
  TSNode root=ts.root_node(ts.tree);
  ts_mark(root);
}
string syntax_face(const string& s,int i,int row){
if(ts.ready && !ts.faces.empty() && row>=0 && row<(int)ts.line_starts.size()){
size_t p=ts.line_starts[row]+(size_t)i;
    if(p<ts.faces.size()){
      if(ts.faces[p]==1) return SYN_COMMENT;
      if(ts.faces[p]==2) return SYN_STRING;
      if(ts.faces[p]==3) return SYN_NUMBER;
      if(ts.faces[p]==4) return SYN_KEYWORD;
      return FG;
    }
  }
  return fallback_syntax_face(s,i);
}
void ts_status(){
  ts_prepare();
  string state;
  if(!tree_sitter_enabled) state="disabled by config";
  else if(ts.ready) state="loaded";
  else state=ts.reason.empty()?string("fallback"):ts.reason;
  msg="Tree-sitter "+state+"; language="+
      (ts.language.empty()?string("none"):ts.language);
}
bool safe_theme_name(const string& name){
  if(name.empty()) return false;
  for(char ch: name)
    if(!isalnum((unsigned char)ch) && ch!='-' && ch!='_') return false;
  return true;
}
string parent_dir(const string& path){
  auto slash=path.find_last_of('/');
  return slash==string::npos?string("."):path.substr(0,slash);
}
bool apply_theme_file(const string& path){
  ifstream f(path);
  if(!f) return false;
  string t((istreambuf_iterator<char>(f)),istreambuf_iterator<char>());
  apply_cfg(t);
  return true;
}
void load_cfg(){
vector<string> paths={"rune.json","nv.json"};
  if(const char* h=getenv("HOME")){
  paths.push_back(string(h)+"/.rune.json");
  paths.push_back(string(h)+"/.nv.json");
  paths.push_back(string(h)+"/.config/rune.json");
  paths.push_back(string(h)+"/.config/nv.json");
  paths.push_back(string(h)+"/.config/rune/config.json");
  paths.push_back(string(h)+"/.config/nv/config.json");
  }
  for(auto& p: paths){
    ifstream f(p);
    if(!f) continue;
    string t((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
    string theme=jstr(t,"theme","");
    if(safe_theme_name(theme)){
      vector<string> theme_paths={
        parent_dir(p)+"/themes/"+theme+".json",
        "themes/"+theme+".json"
      };
      if(const char* h=getenv("HOME"))
        theme_paths.push_back(string(h)+"/.config/nv/themes/"+theme+".json");
      for(auto& tp: theme_paths) if(apply_theme_file(tp)) break;
    }
    apply_cfg(t);
    load_custom_commands(t);
    break;
  }
}

string ext_of(){
  auto d=fname.find_last_of('.');
  if(d==string::npos||d==fname.size()-1) return "all";
  return fname.substr(d+1);
}
void parse_snip_file(const string& t){
  string line; Snip cur; int in=0;
  auto flush=[&](){
    if(in && !cur.trig.empty()){
      if(!cur.body.empty() && cur.body.back()=='\n') cur.body.pop_back();
      snips.push_back(cur);
    }
    cur=Snip{}; in=0;
  };
  for(size_t i=0,n=t.size(); i<=n; i++){
    if(i==n || t[i]=='\n'){
      if(line.rfind("snippet ",0)==0){
        flush(); in=1;
        string rest=line.substr(8);
        size_t a=0; while(a<rest.size() && isspace((unsigned char)rest[a])) a++;
        size_t b=a;
        if(b<rest.size() && (rest[b]=='\''||rest[b]=='"')){
          char q=rest[b++]; while(b<rest.size() && rest[b]!=q) b++;
          cur.trig=rest.substr(a+1,b-a-1); if(b<rest.size()) b++;
        } else {
          while(b<rest.size() && !isspace((unsigned char)rest[b])) b++;
          cur.trig=rest.substr(a,b-a);
        }
        string opt=rest.substr(b);
        if(opt.find('b')!=string::npos) cur.only_bol=1;
        if(opt.find('A')!=string::npos) cur.autoexp=1;
      } else if(line=="endsnippet") flush();
      else if(in){ cur.body+=line; cur.body+='\n'; }
      line.clear();
    } else line+=t[i];
  }
  flush();
}
void load_snips(){
  snips.clear();
  vector<string> paths={"snippets.snippets","all.snippets"};
  if(const char* h=getenv("HOME")){
    string home=h, e=ext_of();
    paths.push_back(home+"/.nv.snippets");
    paths.push_back(home+"/.config/nv/snippets.snippets");
    paths.push_back(home+"/.config/nv/all.snippets");
    paths.push_back(home+"/.config/nv/snippets/all.snippets");
    paths.push_back(home+"/.config/nv/snippets/"+e+".snippets");
    paths.push_back(home+"/.vim/UltiSnips/all.snippets");
    paths.push_back(home+"/.vim/UltiSnips/"+e+".snippets");
    paths.push_back(home+"/.config/nvim/UltiSnips/all.snippets");
    paths.push_back(home+"/.config/nvim/UltiSnips/"+e+".snippets");
  }
  for(auto& p: paths){
    ifstream f(p);
    if(!f) continue;
    string t((istreambuf_iterator<char>(f)), istreambuf_iterator<char>());
    parse_snip_file(t);
  }
  // Project-local snippets are loaded after global/user files, so a project
  // can override a shared trigger for its own file type.
  string e=ext_of();
  vector<string> local={
    ".rune/snippets/all.snippets",
    ".rune/snippets/"+e+".snippets",
    "snippets/"+e+".snippets"
  };
  for(auto& p: local){
    ifstream f(p);
    if(!f) continue;
    string t((istreambuf_iterator<char>(f)),istreambuf_iterator<char>());
    parse_snip_file(t);
  }
  msg="snippets "+to_string((int)snips.size());
}

string shell_quote(const string& s){
  string q="'";
  for(char c:s) { if(c=='\'') q+="'\\''"; else q+=c; }
  return q+"'";
}
void load_custom_commands(const string& t){
  custom_commands.clear();
  size_t p=t.find("\"commands\"");
  if(p==string::npos) return;
  size_t a=t.find('{',p), b=a;
  int depth=0;
  for(;b<t.size();b++){
    if(t[b]=='{') depth++;
    else if(t[b]=='}' && --depth==0) break;
  }
  if(a==string::npos||b==string::npos) return;
  string body=t.substr(a,b-a+1);
  size_t i=1;
  while(i<body.size()){
    while(i<body.size()&&(isspace((unsigned char)body[i])||body[i]==',')) i++;
    if(i>=body.size()||body[i]!='\"') break;
    size_t k=++i; while(i<body.size()&&body[i]!='\"') i++;
    string name=body.substr(k,i-k); size_t c=body.find(':',i);
    if(c==string::npos||c>=body.size()) break;
    i=c+1;
    while(i<body.size()&&isspace((unsigned char)body[i])) i++;
    CustomCommand cmd;
    auto readstr=[&](size_t& q)->string{
      if(q>=body.size()||body[q]!='\"') return "";
      ++q; string out;
      while(q<body.size()&&body[q]!='\"'){ if(body[q]=='\\'&&q+1<body.size()) q++; out+=body[q++]; }
      if(q<body.size()) q++;
      return out;
    };
    if(i<body.size()&&body[i]=='\"') cmd.shell=readstr(i);
    else if(i<body.size()&&body[i]=='{'){
      size_t e=body.find('}',i); if(e==string::npos) break;
      string obj=body.substr(i,e-i+1);
      size_t sk=obj.find("\"shell\""), ck=obj.find("\"cwd\"");
      auto objstr=[&](size_t key)->string{
        if(key==string::npos) return "";
        size_t colon=obj.find(':',key), q=obj.find('\"',colon+1);
        if(q==string::npos) return "";
        string out; ++q;
        while(q<obj.size()&&obj[q]!='\"'){
          if(obj[q]=='\\'&&q+1<obj.size()) ++q;
          out+=obj[q++];
        }
        return out;
      };
      cmd.shell=objstr(sk);
      cmd.cwd=objstr(ck);
      i=e+1;
    } else { i++; continue; }
    if(!name.empty()&&!cmd.shell.empty()) custom_commands[name]=cmd;
  }
}

int rd1(){ unsigned char c; return read(0,&c,1)==1?(int)c:-1; }
int rd_wait(int ms){
  pollfd p{0,POLLIN,0};
  if(poll(&p,1,ms)<=0) return -1;
  return rd1();
}
int readk(){
  int c=rd1();
  if(c!=27) return c=='\r'?'\n':c;
  pollfd p{0,POLLIN,0};
  int wait = poll(&p,1,0)>0 ? 0 : max(25,esc_ms);
  int a=rd_wait(wait); if(a<0) return 27;
  if(a=='b') return K_WORD_LEFT;
  if(a=='f') return K_WORD_RIGHT;
  if(a=='O'){
    int b=rd_wait(max(25,wait)); if(b<0) return 27;
    if(b=='D') return K_LEFT;
    if(b=='C') return K_RIGHT;
    if(b>='0'&&b<='9'){
      string seq(1,(char)b); int d;
      while((d=rd_wait(max(25,wait)))>=0 && !(d>=0x40&&d<=0x7e))
        if(seq.size()<16) seq.push_back((char)d);
      int modifier=atoi(seq.c_str());
      size_t semi=seq.rfind(';');
      if(semi!=string::npos) modifier=atoi(seq.c_str()+semi+1);
      bool shortcut=((modifier-1)&(4|8))!=0 || modifier==5 || modifier==9;
      if(shortcut&&d=='D') return K_WORD_LEFT;
      if(shortcut&&d=='C') return K_WORD_RIGHT;
    }
    return 27;
  }
  if(a=='['){
    int seq_wait=max(25,wait);
    int b=rd_wait(seq_wait); if(b<0) return 27;
    if(b=='A') return K_UP;
    if(b=='B') return K_DOWN;
    if(b=='C') return K_RIGHT;
    if(b=='D') return K_LEFT;
    if(b=='H') return K_HOME;
    if(b=='F') return K_END;
    if(b=='Z') return K_STAB;
    if(b=='<'){
      int btn=0,x=0,y=0,ch;
      while((ch=rd_wait(8))>='0'&&ch<='9') btn=btn*10+ch-'0';
      while((ch=rd_wait(8))>='0'&&ch<='9') x=x*10+ch-'0';
      while((ch=rd_wait(8))>='0'&&ch<='9') y=y*10+ch-'0';
      if(btn==64) return K_WHEEL_UP;
      if(btn==65) return K_WHEEL_DOWN;
      int button=btn&3, modifiers=btn&(4|8|16);
      if(ch=='M' && (btn&32)==0 && (button==0||button==2)){
        click_sx=max(1,x); click_sy=max(1,y);
        click_mod=modifiers?modifiers:(button==2?64:0);
        return K_CLICK;
      }
      return 0;
    }
    if(b>='0'&&b<='9'){
      string seq(1,(char)b);
      int d;
      while((d=rd_wait(seq_wait))>=0 && !(d>=0x40&&d<=0x7e))
        if(seq.size()<32) seq.push_back((char)d);
      vector<int> params;
      size_t at=0;
      while(at<=seq.size()){
        size_t end=seq.find(';',at);
        string part=seq.substr(at,end==string::npos?seq.size()-at:end-at);
        params.push_back(part.empty()?0:atoi(part.c_str()));
        if(end==string::npos) break;
        at=end+1;
      }
      int n=params.empty()?0:params[0];
      int modifier=params.size()>1?params.back():1;
      bool shortcut=((modifier-1)&(4|8))!=0 || (params.size()==1&&(n==5||n==9));
      if(shortcut && d=='D') return K_WORD_LEFT;
      if(shortcut && d=='C') return K_WORD_RIGHT;
      if(d=='~'){
        if(n==3) return K_DEL;
        if(n==5) return K_PGUP;
        if(n==6) return K_PGDN;
        if(n==1) return K_HOME;
        if(n==4) return K_END;
        if(n==200) return K_PASTE_BEGIN;
        if(n==201) return K_PASTE_END;
      }
    }
  }
  return 27;
}

int nlines(){ return (int)L.size(); }
string& ln(){ return L[cy]; }
int vis_w(char ch, int rx){ return ch=='\t' ? tabstop-(rx%tabstop) : 1; }
int cx_to_rx(const string& s, int n){
  int rx=0; n=min(n,(int)s.size());
  for(int i=0;i<n;i++) rx+=vis_w(s[i],rx);
  return rx;
}
int rx_to_cx(const string& s, int trx){
  int rx=0;
  for(int i=0;i<(int)s.size();i++){
    int w=vis_w(s[i],rx);
    if(trx<=rx) return i;
    if(trx<rx+w) return (trx-rx)<=w/2 ? i : i+1;
    rx+=w;
  }
  return (int)s.size();
}
void clampx(){
  int m=(int)ln().size();
  if(mode==NORM && m) m=max(0,m-1);
  cx=max(0,min(cx,m));
}
void load(const string& p){
  fname=p; L.clear(); ifstream f(p);
  if(f){ string s; while(getline(f,s)){ if(!s.empty()&&s.back()=='\r') s.pop_back(); L.push_back(s); } }
  if(L.empty()) L.push_back("");
  cy=cx=off=coff=0; dirty=0;
  reset_cursors();
}
int save(){
if(scratch_buffer){ msg="read-only buffer (:q to close)"; return 0; }
  if(fname.empty()){ msg="No filename (:w name)"; return 0; }
  ofstream f(fname); if(!f){ msg="Can't write"; return 0; }
  for(auto& s: L) f<<s<<'\n';
  dirty=0; msg="wrote "+fname+" "+to_string(nlines())+"L"; return 1;
}

void open_scratch(const string& name, vector<string> lines){
if(!scratch_buffer){
  saved_buffer={L,fname,cx,cy,off,coff,dirty};
}
if(lines.empty()) lines.push_back("");
L=move(lines);
fname=name;
cx=cy=off=coff=0;
dirty=0;
mode=NORM;
sess=Sess{};
shell_popup.clear();
scratch_buffer=true;
msg=":q closes this buffer";
}

void close_scratch(){
if(!scratch_buffer) return;
L=move(saved_buffer.lines);
fname=move(saved_buffer.name);
cx=saved_buffer.x;
cy=saved_buffer.y;
off=saved_buffer.top;
coff=saved_buffer.left;
dirty=saved_buffer.changed;
saved_buffer=SavedBuffer{};
scratch_buffer=false;
mode=NORM;
sess=Sess{};
msg="returned to "+(fname.empty()?string("[No Name]"):fname);
}

void open_help(){
vector<string> lines={
  "NV HELP",
  "",
  "Press :q or q to close this help buffer.",
  "",
  "COMMAND MODE  (press : from normal mode)",
  ""
};
for(int i=0;i<NHELP;i++){
  const CHelp& h=CHELP[i];
  string command=":"+string(h.usage);
  command.append(max(2,24-(int)command.size()),' ');
  command+=h.doc;
  lines.push_back(command);
  if(h.alias && *h.alias) lines.push_back("    aliases: "+string(h.alias));
  if(h.extra && *h.extra) lines.push_back("    "+string(h.extra));
}
vector<string> keys={
  "",
  "NORMAL MODE",
  "h/j/k/l or arrows       move the cursor",
  "w / b                   next / previous word",
  "0 / $                   start / end of line",
  "gg / G                  start / end of file",
  "i / a / I / A           enter insert mode",
  "o / O                   open a line below / above",
  "x / D / dd / dw / daw   delete text",
  "Ctrl-B / Ctrl-F         page up / down",
  "Ctrl-Y / Ctrl-E         scroll up / down",
  "Ctrl-S                  save",
  "Ctrl-N / Ctrl-X          add / skip multicursor occurrence",
  "Ctrl-P                  fuzzy file picker",
  "Ctrl-W h/j/k/l           focus split panes",
  "",
  "INSERT MODE",
  "Escape                  return to normal mode",
  "Tab                     expand snippet or insert configured tab",
  "Shift-Tab               previous snippet stop",
  "Command-V / paste       paste with configured tab behavior",
  "",
  "COMMAND-LINE COMPLETION",
  "Tab                     complete a unique command",
  "Escape                  cancel command entry"
};
lines.insert(lines.end(),keys.begin(),keys.end());
open_scratch("[Help]",move(lines));
}

void open_snippet_list(){
string source=scratch_buffer?saved_buffer.name:fname;
string filetype="all";
auto dot=source.find_last_of('.');
if(dot!=string::npos && dot+1<source.size()) filetype=source.substr(dot+1);
vector<string> lines={
  "SNIPPETS — "+filetype,
  "",
  to_string(snips.size())+" loaded snippet"+(snips.size()==1?"":"s"),
  ""
};
for(auto& sn: snips){
  string flags;
  if(sn.only_bol) flags+=" [line-start]";
  if(sn.autoexp) flags+=" [auto]";
  string preview=sn.body.substr(0,sn.body.find('\n'));
  if(preview.size()>58) preview.resize(58);
  string line=sn.trig+flags;
  line.append(max(2,24-(int)line.size()),' ');
  line+=preview;
  lines.push_back(line);
}
if(snips.empty()) lines.push_back("No snippets found for this filetype.");
open_scratch("[Snippets: "+filetype+"]",move(lines));
}
bool isw(char c){ return isalnum((unsigned char)c)||c=='_'; }

void mv_left(){
  if(cx>0) cx--;
  else if(cy>0){ cy--; cx=(int)ln().size(); if(mode==NORM && cx) cx--; }
}
void mv_right(){
  int m=(int)ln().size();
  int last = mode==INS ? m : max(0,m-1);
  if(cx<last) cx++;
  else if(cy+1<nlines()){ cy++; cx=0; }
}
void mv_vert(int dir){ cy=max(0,min(nlines()-1, cy+dir)); clampx(); }
void page(int dir){
  int h=max(1,rows-1);
  off=max(0,min(max(0,nlines()-h), off+dir*h));
  cy=max(0,min(nlines()-1, cy+dir*h));
  clampx();
}
void scroll_lines(int n){
  int h=max(1,rows-1);
  off=max(0,min(max(0,nlines()-h), off+n));
  if(cy<off) cy=off;
  if(cy>=off+h) cy=off+h-1;
  clampx();
}
void click_to(){
  if(click_sy>=rows) return;
  bool multi=click_mod!=0;
  int old_pane=active_pane, x0=1, y0=1;
  if(panes.size()==2){
    save_active_pane();
    if(split_vertical){
      int w0=max(10,cols/2);
      active_pane=click_sx<=w0?0:1;
      x0=active_pane? w0+2:1;
    } else {
      int h=max(1,rows-1), h0=max(3,h/2);
      active_pane=click_sy<=h0?0:1;
      y0=active_pane? h0+2:1;
    }
    load_active_pane();
    if(active_pane!=old_pane){ reset_cursors(); multi=false; }
  }
  int gutter=panes.size()==2
    ?(int)to_string(max(1,nlines())).size()+2
    :(int)to_string(max(1,nlines())).size()+3;
  int row=off+(click_sy-y0);
  row=max(0,min(row,nlines()-1));
  int trx=max(0,click_sx-x0-gutter)+coff;
  int target_x=rx_to_cx(L[row],trx);
  if(mode==NORM && !L[row].empty()) target_x=min(target_x,(int)L[row].size()-1);
  if(multi){
    sync_primary();
    auto it=find_if(cursors.begin()+min<size_t>(1,cursors.size()),cursors.end(),
      [&](const MultiCursor& c){ return c.y==row&&c.x==target_x; });
    if(it!=cursors.end()) cursors.erase(it);
    else cursors.push_back({row,target_x});
    normalize_cursors();
    msg=to_string(cursors.size())+" cursors";
  } else {
    cy=row; cx=target_x; clampx(); reset_cursors();
  }
}

void word_fwd(){
  if(cx>=(int)ln().size() && cy+1<nlines()){ cy++; cx=0; }
  string& s=ln(); int n=(int)s.size();
  while(cx<n && isw(s[cx])) cx++;
  while(cx<n && !isw(s[cx])) cx++;
  if(cx>=n && cy+1<nlines()){ cy++; cx=0; }
  clampx();
}
void word_back(){
  if(cx==0 && cy>0){ cy--; cx=(int)ln().size(); }
  string& s=ln();
  if(cx>0) cx--;
  while(cx>0 && !isw(s[cx])) cx--;
  while(cx>0 && isw(s[cx-1])) cx--;
}
void del_line(){
  sess=Sess{};
  if(nlines()==1){ L[0].clear(); cx=0; dirty=1; return; }
  L.erase(L.begin()+cy);
  if(cy>=nlines()) cy=nlines()-1;
  cx=0; dirty=1;
}
void daw(){
  string& s=ln(); int n=(int)s.size();
  if(!n){ del_line(); return; }
  int i=min(cx,max(0,n-1)), a=i, b=i+1;
  if(isw(s[i])){
    while(a>0 && isw(s[a-1])) a--;
    while(b<n && isw(s[b])) b++;
    while(b<n && isspace((unsigned char)s[b])) b++;
  } else if(isspace((unsigned char)s[i])){
    while(a>0 && isspace((unsigned char)s[a-1])) a--;
    while(b<n && isspace((unsigned char)s[b])) b++;
    while(b<n && isw(s[b])) b++;
  } else {
    while(a>0 && !isw(s[a-1]) && !isspace((unsigned char)s[a-1])) a--;
    while(b<n && !isw(s[b]) && !isspace((unsigned char)s[b])) b++;
    while(b<n && isspace((unsigned char)s[b])) b++;
  }
  s.erase(a,b-a); cx=a; dirty=1; clampx();
}
void dw(){
  string& s=ln(); int n=(int)s.size();
  if(cx>=n){ if(cy+1<nlines()){ L[cy]+=L[cy+1]; L.erase(L.begin()+cy+1); dirty=1; } return; }
  int b=cx;
  if(isw(s[b])) while(b<n && isw(s[b])) b++;
  else if(isspace((unsigned char)s[b])) while(b<n && isspace((unsigned char)s[b])) b++;
  else while(b<n && !isw(s[b]) && !isspace((unsigned char)s[b])) b++;
  while(b<n && isspace((unsigned char)s[b])) b++;
  s.erase(cx,b-cx); dirty=1; clampx();
}
void del_char(){
  string& s=ln();
  if(cx<(int)s.size()){ s.erase(cx,1); dirty=1; clampx(); }
}
void ins_ch(char c){ ln().insert(ln().begin()+cx,c); cx++; dirty=1; }
void ins_tab(){
  if(expandtab){
    int rx=cx_to_rx(ln(),cx);
    int n=tabstop-(rx%tabstop);
    while(n--) ins_ch(' ');
  } else ins_ch('\t');
}
void split(){
  string t=ln().substr(cx); ln().erase(cx);
  L.insert(L.begin()+cy+1,t); cy++; cx=0; dirty=1;
}
void bs(){
  if(cx>0){ ln().erase(cx-1,1); cx--; dirty=1; }
  else if(cy>0){ cx=(int)L[cy-1].size(); L[cy-1]+=ln(); L.erase(L.begin()+cy); cy--; dirty=1; }
}

void reset_cursors(){ cursors.clear(); cursors.push_back({cy,cx}); }
void sync_primary(){
  if(cursors.empty()) reset_cursors();
  cursors[0]={cy,cx};
}
void normalize_cursors(){
  sort(cursors.begin(),cursors.end(),[](const MultiCursor&a,const MultiCursor&b){
    return a.y!=b.y?a.y<b.y:a.x<b.x;
  });
  cursors.erase(unique(cursors.begin(),cursors.end(),[](const MultiCursor&a,const MultiCursor&b){
    return a.y==b.y&&a.x==b.x;
  }),cursors.end());
  if(cursors.empty()) reset_cursors();
}
string cursor_word(){
  if(cy<0||cy>=nlines()) return "";
  int a=cx,b=cx; const string& s=L[cy];
  if(a==(int)s.size()&&a) a--;
  if(a<(int)s.size()&&!isw(s[a])) return "";
  while(a>0&&isw(s[a-1])) a--;
  while(b<(int)s.size()&&isw(s[b])) b++;
  return s.substr(a,b-a);
}
void add_next_cursor(bool skip=false){
  string w=cursor_word(); if(w.empty()) return;
  int start=skip?cx+1:0;
  for(int y=cy;y<nlines();y++){
    int from=y==cy?start:0;
    for(size_t p=L[y].find(w,(size_t)from); p!=string::npos; p=L[y].find(w,p+1)){
      bool exists=false; for(auto c:cursors) if(c.y==y&&c.x==(int)p) exists=true;
      if(!exists){ cursors.push_back({y,(int)p}); normalize_cursors(); msg=to_string(cursors.size())+" cursors"; return; }
    }
  }
  for(int y=0;y<=cy;y++){
    for(size_t p=L[y].find(w); p!=string::npos; p=L[y].find(w,p+1)){
      bool exists=false; for(auto c:cursors) if(c.y==y&&c.x==(int)p) exists=true;
      if(!exists){ cursors.push_back({y,(int)p}); normalize_cursors(); return; }
    }
  }
}
void multi_insert(char ch){
  normalize_cursors();
  for(int i=(int)cursors.size()-1;i>=0;i--){
    auto c=cursors[i]; if(c.y<(int)L.size()&&c.x<=(int)L[c.y].size()) L[c.y].insert((size_t)c.x,1,ch);
    for(int j=0;j<(int)cursors.size();j++) if(j!=i&&cursors[j].y==c.y&&cursors[j].x>=c.x) cursors[j].x++;
    cursors[i].x++;
  }
  cy=cursors[0].y; cx=cursors[0].x; dirty=1;
}
void multi_backspace(){
  normalize_cursors();
  for(int i=(int)cursors.size()-1;i>=0;i--){
    auto c=cursors[i];
    if(c.x>0){ L[c.y].erase((size_t)c.x-1,1); for(int j=0;j<(int)cursors.size();j++) if(j!=i&&cursors[j].y==c.y&&cursors[j].x>=c.x) cursors[j].x--; cursors[i].x--; }
  }
  cy=cursors[0].y; cx=cursors[0].x; dirty=1;
}
void multi_insert_text(const string& data){
  normalize_cursors();
  for(int n=(int)cursors.size()-1;n>=0;n--){
    int y=cursors[n].y,x=cursors[n].x; if(y<0||y>=(int)L.size()) continue;
    string before=L[y].substr(0,x), after=L[y].substr(x);
    vector<string> parts; string part;
    for(char ch:data){ if(ch=='\n'){ parts.push_back(part); part.clear(); } else part+=ch; }
    parts.push_back(part);
    if(parts.size()==1) {
      L[y]=before+parts[0]+after;
      for(int j=0;j<(int)cursors.size();j++) if(j!=n&&cursors[j].y==y&&cursors[j].x>=x) cursors[j].x+=parts[0].size();
      cursors[n].x+=parts[0].size();
    }
    else {
    L[y]=before+parts[0]; L.insert(L.begin()+y+1,parts.begin()+1,parts.end());
      L[y+parts.size()-1]+=after; cursors[n].y+=parts.size()-1; cursors[n].x=parts.back().size();
    for(int j=0;j<(int)cursors.size();j++) if(j!=n && cursors[j].y>y) cursors[j].y+=(int)parts.size()-1;
    }
  }
  cy=cursors[0].y; cx=cursors[0].x; dirty=1;
}
void multi_delete(){
  normalize_cursors();
  for(int n=(int)cursors.size()-1;n>=0;n--){ auto c=cursors[n]; if(c.y<(int)L.size()&&c.x<(int)L[c.y].size()){ L[c.y].erase(c.x,1); for(int j=0;j<(int)cursors.size();j++) if(j!=n&&cursors[j].y==c.y&&cursors[j].x>c.x) cursors[j].x--; } }
  cy=cursors[0].y; cx=cursors[0].x; dirty=1;
}
void multi_delete_line(){
  normalize_cursors(); set<int> ys; for(auto c:cursors) ys.insert(c.y);
  for(auto it=ys.rbegin();it!=ys.rend();++it) if(*it>=0&&*it<(int)L.size()&&L.size()>1) L.erase(L.begin()+*it);
  if(L.empty()) L.push_back("");
  cy=min(cy,(int)L.size()-1); cx=0; dirty=1;
  vector<MultiCursor> kept;
  for(auto c:cursors){ if(c.y>=(int)L.size()) c.y=L.size()-1; c.x=min(c.x,(int)L[c.y].size()); kept.push_back(c); }
  cursors=kept; if(cursors.empty()) reset_cursors();
}
void multi_word_delete(bool around){
  normalize_cursors();
  for(int n=(int)cursors.size()-1;n>=0;n--){ auto c=cursors[n]; if(c.y>=(int)L.size()) continue; string&s=L[c.y]; int a=c.x,b=c.x;
    while(a>0&&isw(s[a-1])) a--;
    while(b<(int)s.size()&&isw(s[b])) b++;
    if(around) while(b<(int)s.size()&&isspace((unsigned char)s[b])) b++;
    s.erase(a,b-a); cursors[n].x=a;
  }
  cy=cursors[0].y; cx=cursors[0].x; dirty=1;
}

void snip_end();

string read_bracketed_paste(){
  const string end="\x1b[201~";
  string data, pending;
  while(true){
    int c=rd1();
    if(c<0) break;
    pending.push_back((char)c);
    while(!pending.empty() && end.compare(0,pending.size(),pending)!=0){
      data.push_back(pending[0]);
      pending.erase(pending.begin());
    }
    if(pending==end) break;
  }
  return data;
}

string read_raw_paste(){
  string data;
  int c;
  while((c=rd_wait(25))>=0) data.push_back((char)c);
  return data;
}

void paste_into_buffer(const string& data){
if(scratch_buffer){ msg="read-only buffer (:q to close)"; return; }
  if(mode!=INS){
    mode=INS;
    clampx();
  }
  snip_end();
  if(cursors.size()>1){ multi_insert_text(data); msg="pasted"; return; }
  for(size_t i=0;i<data.size();i++){
    unsigned char ch=(unsigned char)data[i];
    if(ch=='\r'){
      if(i+1<data.size() && data[i+1]=='\n') i++;
      split();
    } else if(ch=='\n'){
      split();
    } else if(ch=='\t'){
      ins_tab();
    } else if(ch>=32 || ch>=128){
      ins_ch((char)ch);
    }
  }
  msg="pasted";
}

void paste_into_command(const string& data){
  for(unsigned char ch: data)
    if(ch>=32 && ch!=127) cbuf.push_back((char)ch);
}

void snip_end(){ sess=Sess{}; }
void adj_stops(int y, int x, int dx){
  for(auto& s: sess.st)
    if(s.y==y && s.x>x) s.x+=dx;
}
void snip_goto(int i){
  if(i<0||i>=(int)sess.st.size()){ snip_end(); return; }
  sess.idx=i;
  Stop& s=sess.st[i];
  cy=max(0,min(nlines()-1,s.y));
  cx=s.x; clampx();
  sess.sel = s.len>0;
  msg="snip "+to_string(i+1)+"/"+to_string((int)sess.st.size());
}
int snip_id_rank(int id){ return id==0 ? 1000 : id; }
void snip_sort(){
  stable_sort(sess.st.begin(), sess.st.end(), [](const Stop& a, const Stop& b){
    int ra=snip_id_rank(a.id), rb=snip_id_rank(b.id);
    if(ra!=rb) return ra<rb;
    if(a.y!=b.y) return a.y<b.y;
    return a.x<b.x;
  });
}
int parse_int(const string& s, size_t& i){
  int n=0; while(i<s.size() && isdigit((unsigned char)s[i])) n=n*10+s[i++]-'0';
  return n;
}
string parse_body(const string& body, int y0, int x0, vector<Stop>& out){
  string r; int y=0,x=0;
  for(size_t i=0;i<body.size();){
    if(body[i]=='\n'){ r+='\n'; y++; x=0; i++; continue; }
    if(body[i]=='$' && i+1<body.size()){
      size_t j=i+1;
      if(body[j]=='{'){
        j++;
        if(j<body.size() && isdigit((unsigned char)body[j])){
          int id=parse_int(body,j);
          string ph;
          if(j<body.size() && body[j]==':'){
            j++;
            int depth=1;
            while(j<body.size() && depth){
              if(body[j]=='{') depth++;
              else if(body[j]=='}'){ depth--; if(!depth) break; }
              else ph+=body[j];
              j++;
            }
            if(j<body.size() && body[j]=='}') j++;
          } else {
            while(j<body.size() && body[j]!='}') j++;
            if(j<body.size()) j++;
          }
          size_t vis=ph.find("${VISUAL}");
          if(vis!=string::npos) ph.erase(vis,9);
          vis=ph.find("$VISUAL");
          if(vis!=string::npos) ph.erase(vis,7);
          int sy=y0+(y==0?0:y), sx=(y==0?x0+x:x);
          out.push_back({id,sy,sx,(int)ph.size()});
          r+=ph; x+=(int)ph.size(); i=j; continue;
        }
      } else if(isdigit((unsigned char)body[j])){
        int id=parse_int(body,j);
        int sy=y0+(y==0?0:y), sx=(y==0?x0+x:x);
        out.push_back({id,sy,sx,0});
        i=j; continue;
      }
    }
    r+=body[i++]; x++;
  }
  return r;
}
void insert_block(const string& t){
  for(char c: t){
    if(c=='\n') split();
    else ins_ch(c);
  }
}
int try_expand(){
  string& s=ln();
  int i=cx;
  while(i>0 && !isspace((unsigned char)s[i-1])) i--;
  string tok=s.substr(i,cx-i);
  if(tok.empty()) return 0;
  int bol=1;
  for(int k=0;k<i;k++) if(!isspace((unsigned char)s[k])) bol=0;
  const Snip* hit=0;
  for(auto& sn: snips){
    if(sn.trig!=tok) continue;
    if(sn.only_bol && !bol) continue;
    hit=&sn;
  }
  if(!hit) return 0;
  s.erase(i,cx-i); cx=i; dirty=1;
  vector<Stop> st;
  string built=parse_body(hit->body, cy, cx, st);
  insert_block(built);
  sess=Sess{}; sess.st=st; snip_sort();
  mode=INS;
  if(sess.st.empty()){ snip_end(); return 1; }
  snip_goto(0);
  return 1;
}
int try_auto(){
  string& s=ln();
  int i=cx;
  while(i>0 && !isspace((unsigned char)s[i-1])) i--;
  string tok=s.substr(i,cx-i);
  if(tok.empty()) return 0;
  int bol=1;
  for(int k=0;k<i;k++) if(!isspace((unsigned char)s[k])) bol=0;
  for(auto& sn: snips){
    if(!sn.autoexp || sn.trig!=tok) continue;
    if(sn.only_bol && !bol) continue;
    return try_expand();
  }
  return 0;
}
void snip_next(int dir){
  if(sess.idx<0) return;
  int n=(int)sess.st.size();
  int j=sess.idx+dir;
  if(j>=n){ snip_end(); return; }
  if(j<0) j=0;
  snip_goto(j);
}
void snip_type(char c){
  if(sess.idx>=0 && sess.sel){
    Stop& s=sess.st[sess.idx];
    if(s.y==cy && s.len>0 && cx>=s.x && cx<=s.x+s.len){
      if(s.x+s.len<=(int)ln().size()) ln().erase(s.x,s.len);
      adj_stops(s.y,s.x,-s.len);
      cx=s.x; s.len=0; sess.sel=0;
    } else sess.sel=0;
  }
  ins_ch(c);
  if(sess.idx>=0){
    Stop& s=sess.st[sess.idx];
    if(s.y==cy && cx-1>=s.x){ s.len++; adj_stops(cy,cx-1,1); }
  }
}

// Two quiet columns on the left give the line numbers the same breathing
// room as the reference terminal.  The returned value includes the gutter.
int gw(){ return (int)to_string(max(1,nlines())).size()+3; }

string first_tok(const string& s){
  size_t a=0; while(a<s.size() && isspace((unsigned char)s[a])) a++;
  size_t b=a; while(b<s.size() && !isspace((unsigned char)s[b])) b++;
  return s.substr(a,b-a);
}
int cmd_match(const CHelp& h, const string& t){
  if(t.empty()) return 1;
  string n=h.name, al=h.alias?h.alias:"";
  return n.rfind(t,0)==0 || (!al.empty() && al.rfind(t,0)==0);
}
void paint_xy(string& o, int y, int x, const string& s, const string& face){
  o+="\x1b["+to_string(y)+";"+to_string(x)+"H";
  o+=face; o+=s; o+="\x1b[0m";
}

void picker_walk(const string& dir, int depth=0){
  if(depth>12||picker_files.size()>=1000) return;
  DIR* d=opendir(dir.c_str()); if(!d) return;
  while(auto* ent=readdir(d)){
    string n=ent->d_name;
    if(n=="."||n==".."||n==".git"||(!n.empty()&&n[0]=='.')) continue;
    string p=dir=="."?n:dir+"/"+n;
    struct stat st{};
    if(stat(p.c_str(),&st)!=0) continue;
    if(S_ISDIR(st.st_mode)) picker_walk(p,depth+1);
    else if(S_ISREG(st.st_mode)) picker_files.push_back(p);
  }
  closedir(d);
}
int fuzzy_score(const string& path,const string& q){
  if(q.empty()) return 0;
  int at=0,score=0,run=0;
  for(char want:q){
    bool found=false;
    for(;at<(int)path.size();at++) if(tolower((unsigned char)path[at])==tolower((unsigned char)want)){
      score+=10+(run++*5)+(at==0||path[at-1]=='/'?12:0); at++; found=true; break;
    }
    if(!found) return -1;
  }
  return score-(int)path.size();
}
vector<string> picker_matches(){
  vector<pair<int,string>> ranked;
  for(auto& p:picker_files){ int s=fuzzy_score(p,picker_query); if(s>=0) ranked.push_back({-s,p}); }
  sort(ranked.begin(),ranked.end());
  vector<string> out; for(auto& x:ranked) out.push_back(x.second);
  return out;
}
void open_picker(){ picker_files.clear(); picker_walk("."); picker_query.clear(); picker_selected=0; picker_open=true; }
void draw_picker(string& o){
  if(!picker_open) return;
  vector<string> m=picker_matches(); int width=min(cols-4,64), h=min(rows-5,12);
  int x=max(1,(cols-width)/2), y=max(1,(rows-h)/2);
  string title=" Find file: "+picker_query+" "; title.resize(min(width,(int)title.size()));
  paint_xy(o,y,x,title,POPUP_SELECTED_FACE);
  for(int i=0;i<h-1;i++){
    string line=i<(int)m.size()?((i==picker_selected?"> ":"  ")+m[i]):"";
    if((int)line.size()>width) line.resize(width);
    line.append(width-line.size(),' ');
    paint_xy(o,y+1+i,x,line,i==picker_selected?POPUP_SELECTED_FACE:POPUP_FACE);
  }
  }
void draw_palette(string& o){
if(!palette_open) return;
vector<string> items={"<Space>","f  file picker","v  vertical split","s  horizontal split",
  "c  close split","h  help","n  snippets list","t  interactive shell",
  "w  save","q  close palette","?  help"};
if(custom_commands.count("make")) items.insert(items.begin()+8,"m  run make");
int width=24, x=cols>width+4?cols-width-2:2;
// Keep the palette compact and close to the status bar, like the
// terminal popup, while leaving one clear row above the status line.
int y=max(2,::rows-(int)items.size()-1);
for(int i=0;i<(int)items.size();i++){
  string line=items[i]; line.resize(min(width,(int)line.size())); line.append(width-line.size(),' ');
  paint_xy(o,y+i,x,line,i==0?POPUP_SELECTED_FACE:POPUP_FACE);
}
}
bool digits_only(const string& s){
  if(s.empty()) return false;
  for(char ch: s) if(!isdigit((unsigned char)ch)) return false;
  return true;
}

void draw_completions(string& o){
  if(mode!=CMD) return;
  string t=first_tok(cbuf);
  vector<string> items;
  if(digits_only(t)){
    items.push_back(":"+t+"  jump to line "+t);
  } else {
    for(int i=0;i<NHELP && (int)items.size()<7;i++){
      if(!cmd_match(CHELP[i],t)) continue;
      string item=":"+string(CHELP[i].usage);
      int pad=max(2,22-(int)item.size());
      item.append(pad,' ');
      item+=CHELP[i].doc;
      items.push_back(item);
    }
    for(auto& kv:custom_commands){
      if(kv.first.rfind(t,0)==0 && (int)items.size()<7)
        items.push_back(":"+kv.first+"  configured shell command");
    }
  }
  if(items.empty()) return;
  int width=max(1,cols);
  int top=max(1,rows-(int)items.size());
  for(int i=0;i<(int)items.size();i++){
    string line=items[i].substr(0,width);
    line.append(width-(int)line.size(),' ');
    paint_xy(o,top+i,1,line,i==0?POPUP_SELECTED_FACE:POPUP_FACE);
  }
}

void draw_shell_popup(string& o){
  if(shell_popup.empty()) return;
  int x=min(cols,max(1,gw()+1));
  int available=max(1,cols-x+1);
  int width=1;
  int count=min(8,(int)shell_popup.size());
  for(int i=0;i<count;i++) width=max(width,(int)shell_popup[i].size());
  width=min(available,min(48,width+2));
  for(int i=0;i<count && i+2<rows;i++){
    string line=" "+shell_popup[i];
    if((int)line.size()>width) line.resize(width);
    line.append(width-(int)line.size(),' ');
    paint_xy(o,2+i,x,line,i==0?POPUP_SELECTED_FACE:POPUP_FACE);
  }
}
void terminal_parser_reset(){
  terminal_cursor_x=terminal_cursor_y=0;
  terminal_parser_state=0;
  terminal_csi_params.clear();
  terminal_osc.clear();
}
int terminal_height() { return max(1,rows-6); }
int terminal_width() { return max(1,cols-8); }
    int terminal_view_top(){
      return max(0,(int)terminal_lines.size()-terminal_height());
    }
void terminal_ensure_line(int y){
  if(y<0) return;
  while((int)terminal_lines.size()<=y) terminal_lines.push_back("");
}
void terminal_clear_line(int y, int from=0){
  terminal_ensure_line(y);
  if(from<0) from=0;
  if((int)terminal_lines[y].size()>from) terminal_lines[y].erase(from);
}
vector<int> terminal_csi_values(){
  vector<int> out; string s=terminal_csi_params;
  if(!s.empty() && (s[0]=='?'||s[0]=='>'||s[0]=='!')) s.erase(0,1);
  size_t at=0;
  while(at<=s.size()){
    size_t end=s.find(';',at);
    string part=s.substr(at,end==string::npos?s.size()-at:end-at);
    out.push_back(part.empty()?0:atoi(part.c_str()));
    if(end==string::npos) break;
    at=end+1;
  }
  return out;
}
void terminal_csi(char final){
  vector<int> p=terminal_csi_values();
  int a=p.empty()||p[0]==0?1:p[0], b=p.size()<2||p[1]==0?1:p[1];
  int top=max(0,(int)terminal_lines.size()-terminal_height());
  if(final=='A') terminal_cursor_y=max(top,terminal_cursor_y-a);
  else if(final=='B') terminal_cursor_y=min(max(top,(int)terminal_lines.size()-1),terminal_cursor_y+a);
  else if(final=='C') terminal_cursor_x=min(terminal_width()-1,terminal_cursor_x+a);
  else if(final=='D') terminal_cursor_x=max(0,terminal_cursor_x-a);
  else if(final=='G') terminal_cursor_x=max(0,min(terminal_width()-1,a-1));
  else if(final=='H'||final=='f'){
    int row=a-1, col=b-1;
    terminal_cursor_y=min(max(top,0)+row,max(top,(int)terminal_lines.size()-1));
    terminal_cursor_x=max(0,min(terminal_width()-1,col));
    terminal_ensure_line(terminal_cursor_y);
  } else if(final=='J'){
    int mode=p.empty()?0:p[0];
    if(mode==2||mode==3){
      terminal_lines.clear(); terminal_lines.push_back("");
      terminal_cursor_x=terminal_cursor_y=0;
    } else if(mode==0){
      terminal_clear_line(terminal_cursor_y,terminal_cursor_x);
      terminal_lines.resize(min(terminal_lines.size(),(size_t)terminal_cursor_y+1));
    }
  } else if(final=='K'){
    int mode=p.empty()?0:p[0];
    terminal_ensure_line(terminal_cursor_y);
    if(mode==0) terminal_clear_line(terminal_cursor_y,terminal_cursor_x);
    else if(mode==1) terminal_lines[terminal_cursor_y].erase(0,min((size_t)terminal_cursor_x,terminal_lines[terminal_cursor_y].size()));
    else if(mode==2) terminal_lines[terminal_cursor_y].clear();
  }
  // SGR (m), private modes, and other unsupported CSI commands are
  // intentionally consumed without affecting the terminal text.
}
void terminal_put(unsigned char ch){
  if(ch=='\r'){ terminal_cursor_x=0; return; }
  if(ch=='\n'){
    terminal_cursor_x=0; terminal_cursor_y++;
    terminal_ensure_line(terminal_cursor_y);
    return;
  }
  if(ch=='\b'){ terminal_cursor_x=max(0,terminal_cursor_x-1); return; }
  if(ch=='\t'){
    terminal_cursor_x=min(terminal_width()-1,((terminal_cursor_x/8)+1)*8);
    return;
  }
  if(ch<32 || ch==127) return;
  terminal_ensure_line(terminal_cursor_y);
  string& line=terminal_lines[terminal_cursor_y];
  if((int)line.size()<terminal_cursor_x) line.append(terminal_cursor_x-line.size(),' ');
  if(terminal_cursor_x>=terminal_width()) return;
  if(terminal_cursor_x==(int)line.size()) line.push_back((char)ch);
  else line[terminal_cursor_x]=(char)ch;
  terminal_cursor_x++;
}
void terminal_drain(){
  if(terminal_fd<0) return;
  char buf[4096]; ssize_t n;
  while((n=read(terminal_fd,buf,sizeof(buf)))>0){
    for(ssize_t i=0;i<n;i++){
      unsigned char ch=(unsigned char)buf[i];
      if(terminal_parser_state==1){
        if(ch=='['){ terminal_parser_state=2; terminal_csi_params.clear(); }
        else if(ch==']'){ terminal_parser_state=3; terminal_osc.clear(); }
        else terminal_parser_state=0;
        continue;
      }
      if(terminal_parser_state==2){
        if(ch>=0x40&&ch<=0x7e){
          terminal_csi(ch); terminal_parser_state=0;
        } else if(terminal_csi_params.size()<64) terminal_csi_params.push_back((char)ch);
        continue;
      }
      if(terminal_parser_state==3){
        if(ch==7){ terminal_parser_state=0; terminal_osc.clear(); }
        else if(ch==27){ terminal_parser_state=1; }
        else if(terminal_osc.size()<256) terminal_osc.push_back((char)ch);
        continue;
      }
      if(ch==27){ terminal_parser_state=1; continue; }
      terminal_put(ch);
    }
  }
  if(n<0 && errno!=EAGAIN && errno!=EWOULDBLOCK){
    close(terminal_fd); terminal_fd=-1;
    if(terminal_pid>0){ waitpid(terminal_pid,nullptr,WNOHANG); terminal_pid=-1; }
    terminal_popup=false; msg="shell exited";
  }
  while(terminal_lines.size()>400){
    terminal_lines.erase(terminal_lines.begin());
    terminal_cursor_y=max(0,terminal_cursor_y-1);
  }
}
void terminal_close(){
  if(terminal_fd>=0){ close(terminal_fd); terminal_fd=-1; }
  if(terminal_pid>0){ kill(terminal_pid,SIGHUP); waitpid(terminal_pid,nullptr,0); terminal_pid=-1; }
  terminal_popup=false;
  terminal_parser_reset();
}
void terminal_start(){
  terminal_close(); terminal_lines.clear(); terminal_lines.push_back("");
  terminal_parser_reset();
  winsize ws{}; ws.ws_row=(unsigned short)max(8,rows-6); ws.ws_col=(unsigned short)max(30,cols-8);
  terminal_pid=forkpty(&terminal_fd,nullptr,nullptr,&ws);
  if(terminal_pid<0){ terminal_fd=-1; msg="could not start terminal"; return; }
  if(terminal_pid==0){
    string shell=terminal_shell;
    if(shell.empty()) if(const char* env=getenv("SHELL")) shell=env;
    if(shell.empty()) shell="/bin/sh";
    execlp(shell.c_str(),shell.c_str(),(char*)nullptr);
    _exit(127);
  }
  int flags=fcntl(terminal_fd,F_GETFL,0);
  fcntl(terminal_fd,F_SETFL,flags|O_NONBLOCK);
  terminal_popup=true; msg="interactive shell";
}
void terminal_resize(){
  if(terminal_fd<0) return;
  winsize ws{};
  ws.ws_row=(unsigned short)max(8,rows-6);
  ws.ws_col=(unsigned short)max(30,cols-8);
  (void)ioctl(terminal_fd,TIOCSWINSZ,&ws);
}
void terminal_send(int k){
  if(terminal_fd<0) return;
  if(k==12){
    terminal_lines.clear(); terminal_lines.push_back("");
    terminal_parser_reset();
  }
  string out;
  if(k==K_UP) out="\x1b[A"; else if(k==K_DOWN) out="\x1b[B";
  else if(k==K_LEFT) out="\x1b[D"; else if(k==K_RIGHT) out="\x1b[C";
  else if(k==K_WORD_LEFT) out="\x1b[1;5D"; else if(k==K_WORD_RIGHT) out="\x1b[1;5C";
  else if(k==K_HOME) out="\x1b[H"; else if(k==K_END) out="\x1b[F";
  else if(k==K_BACK||k==8) out="\x7f";
  else if(k>=0&&k<256) out.push_back((char)k);
  if(!out.empty()) (void)!write(terminal_fd,out.data(),out.size());
}
void draw_terminal_popup(string& o){
  if(!terminal_popup) return;
  terminal_resize();
  terminal_drain();
  int x=4,y=2,w=max(20,cols-8),h=terminal_height();
  int top=terminal_view_top();
  for(int i=0;i<h;i++){
    int idx=top+i;
    string line=idx<(int)terminal_lines.size()?terminal_lines[idx]:string();
    if((int)line.size()>w) line.resize(w);
    line.append(w-line.size(),' ');
    paint_xy(o,y+i,x,line,POPUP_FACE);
  }
}

string mode_label(){
  if(mode==INS) return "INS";
  if(mode==CMD) return "CMD";
  return "NOR";
}

void draw_status(string& o){
  string left, right;
  if(mode==CMD){
  left=":"+cbuf;
  } else {
  left=mode_label()+"  "+(fname.empty()?string("[No Name]"):fname);
  if(scratch_buffer) left+=" [RO]";
    if(dirty) left+=" [+]";
  right=to_string(cy+1)+" sel "+to_string(cx+1)+":"+to_string(cx+1);
  if(sess.idx>=0) right+="  ${"+to_string(sess.st[sess.idx].id)+"}";
  if(cursors.size()>1) right+="  "+to_string(cursors.size())+" cursors";
  if(split_vertical) right+="  [vsplit]";
  if(split_horizontal) right+="  [split]";
  if(!msg.empty()) right+="  "+msg;
  }
if((int)left.size()+(int)right.size()+1>cols){
  int room=max(0,cols-(int)left.size()-1);
  if((int)right.size()>room) right=right.substr(right.size()-room);
}
o+=BG_BAR;
o+="\x1b["+to_string(rows)+";1H"+FG_BRIGHT+left;
int spaces=max(1,cols-(int)left.size()-(int)right.size());
o.append(spaces,' '); o+=right;
o.append(max(0,cols-(int)left.size()-spaces-(int)right.size()),' ');
o+=RESET;
}

void draw_pane_region(string& o,int x0,int y0,int w,int h,bool active){
  ts_prepare();
  int g=(int)to_string(max(1,nlines())).size()+2;
  off=max(0,min(off,max(0,nlines()-h)));
  if(cy<off) off=cy;
  if(cy>=off+h) off=cy-h+1;
  for(int i=0;i<h;i++){
    int r=off+i;
    o+="\x1b["+to_string(y0+i)+";"+to_string(x0)+"H";
    o+=BG;
    if(r>=nlines()){ o+=FG_FAINT+"~"; o.append(max(0,w-1),' '); o+=RESET; continue; }
    string num=to_string(r+1);
    o+=FG_DIM; o.append(max(0,g-(int)num.size()-1),' '); o+=num+" ";
    int used=g;
    const string& s=L[r];
    for(int j=coff;j<(int)s.size()&&used<w;j++){
      char ch=s[j];
      if(active&&cursors.size()>1){
        bool caret=false;
        for(const auto& c:cursors) if(c.y==r&&c.x==j) caret=true;
        if(caret) o+="\x1b[7m";
      }
      o+=syntax_face(s,j,r); o+=(ch=='\t'?' ':ch); o+=RESET+BG; used++;
    }
    if(active&&cursors.size()>1&&used<w){
      bool caret=false;
      for(const auto& c:cursors) if(c.y==r&&c.x==(int)s.size()) caret=true;
      if(caret){ o+="\x1b[7m \x1b[0m"+BG; used++; }
    }
    o.append(max(0,w-used),' '); o+=RESET;
  }
}
void draw_split_layout(string& o){
  int ap=active_pane;
  save_active_pane();
  int h=max(1,rows-1);
  int first=active_pane==0?0:0;
  (void)first;
  if(split_vertical){
    int w0=max(10,cols/2), w1=max(10,cols-w0-1);
    active_pane=0; load_active_pane(); draw_pane_region(o,1,1,w0,h,ap==0);
    active_pane=1; load_active_pane(); draw_pane_region(o,w0+2,1,w1,h,ap==1);
  } else {
    int h0=max(3,h/2), h1=max(3,h-h0-1);
    active_pane=0; load_active_pane(); draw_pane_region(o,1,1,cols,h0,ap==0);
    active_pane=1; load_active_pane(); draw_pane_region(o,1,h0+2,cols,h1,ap==1);
  }
  active_pane=ap;
  load_active_pane();
  draw_status(o);
}
void create_split(bool vertical,const string& file){
  save_active_pane();
  if(panes.empty()) panes.push_back(capture_pane());
  PaneState second=panes[active_pane];
  if(!file.empty()){
    vector<string> old=L; string oldn=fname; int ox=cx,oy=cy,oo=off,ol=coff,od=dirty;
    load(file); second=capture_pane();
    L=old; fname=oldn; cx=ox; cy=oy; off=oo; coff=ol; dirty=od;
  }
  panes.resize(2); panes[1]=second;
  split_vertical=vertical; split_horizontal=!vertical; active_pane=1; load_active_pane();
  cursors.clear(); reset_cursors(); msg=vertical?":vsplit":" :split";
}
void draw(){
  winsz();
  if(panes.size()==2){
    string o="\x1b[?25l\x1b[?7l"+BG;
    draw_split_layout(o);
  draw_palette(o);
    draw_terminal_popup(o);
    int ap=active_pane; load_active_pane();
    int h=max(1,rows-1);
    int pane_x=split_vertical?(ap?cols/2+2:1):1;
    int px=pane_x+(int)to_string(max(1,nlines())).size()+2;
    int py=split_horizontal?(ap?max(3,h/2)+2:1):1;
    int rx=cx_to_rx(ln(),cx);
    px+=rx-coff;
    if(terminal_popup){
      int ty=2+terminal_cursor_y-terminal_view_top();
      int tx=4+terminal_cursor_x;
      o+="\x1b["+to_string(min(rows,max(1,ty)))+";"+to_string(min(cols,max(1,tx)))+"H";
      o+="\x1b[6 q";
    } else {
      o+="\x1b["+to_string(min(rows,max(1,py+cy-off)))+";"+to_string(min(cols,max(1,px)))+"H";
      o+=(mode==NORM?"\x1b[2 q":"\x1b[6 q");
    }
    o+="\x1b[?7h";
    o+=(terminal_popup||cursors.size()<=1?"\x1b[?25h":"\x1b[?25l");
    (void)!write(1,o.data(),o.size());
    return;
  }
  int h=max(1,rows-1), g=gw(), tw=max(1,cols-g-1);
  ts_prepare();
  int so=min(scrolloff, max(0,h/2));
  if(cy<off+so) off=max(0,cy-so);
  if(cy>=off+h-so) off=cy-h+1+so;
  if(off<0) off=0;
  int rx=cx_to_rx(ln(),cx);
  if(rx<coff) coff=rx;
  if(rx>=coff+tw) coff=rx-tw+1;
  string o; o.reserve((size_t)cols*rows+256);
  o+="\x1b[?25l\x1b[?7l";
  o+=BG;
  o+="\x1b[1;1H";
  for(int i=0;i<h;i++){
    int r=i+off;
    o+=BG;
    int used=0;
    if(r<nlines()){
      string nb=to_string(r+1);
      nb=string(max(0,g-1-(int)nb.size()),' ')+nb+' ';
      if((int)nb.size()>cols) nb.resize(cols);
      o+=FG_DIM; o+=nb; o+=FG;
      used+=(int)nb.size();
      const string& s=L[r];
      int col=0;
      for(int si=0;si<(int)s.size();si++){
        char ch=s[si];
        int w=vis_w(ch,col);
        for(int k=0;k<w;k++){
          if(col+k>=coff && col+k<coff+tw && used<cols){
            bool caret=false;
            if(cursors.size()>1)
              for(const auto& c:cursors)
                if(c.y==r&&c.x==si) caret=true;
            o+=(caret?"\x1b[7m":syntax_face(s,si,r));
            o+=(ch=='\t'?' ':ch);
            o+=RESET; o+=BG;
            used++;
          }
        }
        col+=w;
      }
      if(cursors.size()>1&&used<cols){
        bool caret=false;
        for(const auto& c:cursors) if(c.y==r&&c.x==(int)s.size()) caret=true;
        if(caret){ o+="\x1b[7m \x1b[0m"+BG; used++; }
      }
    } else {
      o+=FG_FAINT;
      o+="~";
      used=1;
    }
    if(used<cols) o.append(cols-used,' ');
    o+=RESET;
    if(i<h-1) o+="\r\n";
  }
  draw_shell_popup(o);
  draw_terminal_popup(o);
  draw_completions(o);
  draw_picker(o);
draw_palette(o);
  draw_status(o);
  int curx, cury;
  if(terminal_popup){
    curx=4+terminal_cursor_x;
    cury=2+terminal_cursor_y-terminal_view_top();
  } else if(mode==CMD){ cury=rows; curx=min(cols,(int)cbuf.size()+2); }
  else { curx=g+1+(rx-coff); cury=cy-off+1; }
  curx=min(cols,max(1,curx)); cury=min(rows,max(1,cury));
  o+="\x1b["+to_string(cury)+";"+to_string(curx)+"H";
  o+=(terminal_popup||mode!=NORM?"\x1b[6 q":"\x1b[2 q");
  o+="\x1b[?7h";
  o+=(terminal_popup||cursors.size()<=1?"\x1b[?25h":"\x1b[?25l");
  (void)!write(1,o.data(),o.size());
}

void run_shell_command(const string& command){
  shell_popup.clear();
  if(command.empty()){
    msg="usage: :sh <command>";
    return;
  }
  string full=command+" 2>&1";
  FILE* pipe=popen(full.c_str(),"r");
  if(!pipe){
    shell_popup.push_back("could not start shell");
    return;
  }
  char buf[1024];
  while(fgets(buf,sizeof(buf),pipe) && shell_popup.size()<200){
    string line=buf;
    while(!line.empty() && (line.back()=='\n'||line.back()=='\r')) line.pop_back();
    shell_popup.push_back(line);
  }
  int status=pclose(pipe);
  if(shell_popup.empty()) shell_popup.push_back("(no output)");
  if(status!=-1 && WIFEXITED(status) && WEXITSTATUS(status)!=0)
    shell_popup.push_back("[exit "+to_string(WEXITSTATUS(status))+"]");
  msg="Esc closes output";
}
void run_custom_command(const string& name){
  auto it=custom_commands.find(name);
  if(it==custom_commands.end()){ msg="E492: unknown command: "+name; return; }
  string cmd=it->second.shell, file=shell_quote(fname.empty()?"":fname);
  size_t p;
  while((p=cmd.find("{file}"))!=string::npos) cmd.replace(p,6,file);
  while((p=cmd.find("{dir}"))!=string::npos) cmd.replace(p,5,shell_quote(parent_dir(fname)));
  string old;
  char cwd[4096]; if(getcwd(cwd,sizeof(cwd))) old=cwd;
  if(it->second.cwd=="project") chdir(old.c_str());
  else if(it->second.cwd=="buffer") chdir(parent_dir(fname).c_str());
  run_shell_command(cmd);
  if(!old.empty()) chdir(old.c_str());
}
bool close_active_split(bool force){
  if(panes.size()!=2){ msg="E444: Cannot close last window"; return false; }
  save_active_pane();
  if(dirty&&!force){ msg="No write since last change (:q! to force)"; return false; }
  int keep=active_pane?0:1;
  PaneState p=panes[keep];
  panes.clear(); panes.push_back(p);
  active_pane=0; split_vertical=split_horizontal=false;
  restore_pane(p); load_snips(); reset_cursors(); msg="closed split";
  return true;
}

int colon(){
  string t=first_tok(cbuf), rest;
  size_t sp=cbuf.find(' ');
  if(sp!=string::npos) rest=cbuf.substr(sp+1);
  while(!rest.empty() && isspace((unsigned char)rest[0])) rest.erase(rest.begin());
  string linearg;
  if(digits_only(t)) linearg=t;
  else if(t=="number"||t=="line") linearg=first_tok(rest);
  if(!linearg.empty() && digits_only(linearg)){
    long long target=0;
    for(char ch: linearg) target=min<long long>(nlines(),target*10+(ch-'0'));
    cy=max(0,min(nlines()-1,(int)max(1LL,target)-1));
    cx=0;
    msg="line "+to_string(cy+1);
    return 0;
  }
  if(t=="end"||t=="$"){
    cy=nlines()-1;
    cx=ln().empty()?0:(int)ln().size()-1;
    msg="end of file";
    return 0;
  }
  if(t=="sh"||t=="shell"){
    if(rest.empty()) terminal_start(); else run_shell_command(rest);
    return 0;
  }
  if(t=="term"||t=="terminal"){
    terminal_start();
    return 0;
  }
  if(t=="split"||t=="sp"||t=="vsplit"||t=="vs"){
    if(rows<8||cols<24){ msg="E36: Not enough room for split"; return 0; }
    create_split(t=="vsplit"||t=="vs",rest);
    return 0;
  }
  if(t=="close"||t=="close!"){
    close_active_split(t=="close!");
    return 0;
  }
  if(t=="picker"){ open_picker(); return 0; }
  if(t=="ts-status"||t=="ts"){ ts_status(); return 0; }
  if(custom_commands.count(t)){ run_custom_command(t); return 0; }
if(t=="help"||t=="h"){
  open_help();
  return 0;
}
if(t=="snippets-ls"||t=="snips"){
  open_snippet_list();
  return 0;
}
if((t=="q"||t=="quit"||t=="q!") && scratch_buffer){
  close_scratch();
  return 0;
}
  if(t=="q"||t=="quit"){
    if(panes.size()==2){ close_active_split(false); return 0; }
    if(dirty){ msg="No write since last change (:q! to force)"; return 0; }
    return 1;
  }
  if(t=="q!" && panes.size()==2){ close_active_split(true); return 0; }
  if(t=="q!" ) return 1;
  if(t=="w"||t=="write"){ if(!rest.empty()) fname=rest; save(); return 0; }
  if(t=="wq"||t=="x"){
    if(!rest.empty()) fname=rest;
    if(!save()) return 0;
    if(panes.size()==2){ close_active_split(true); return 0; }
    return 1;
  }
  if(t=="e"||t=="edit"){
    if(rest.empty()){ msg="E32: No file name"; return 0; }
    load(rest); load_snips(); msg="opened "+fname; return 0;
  }
  if(t=="snippets"||t=="snip"){ load_snips(); return 0; }
  msg="E492: Not an editor command: "+t; return 0;
}

void do_move_key(int k){
  sess.sel=0;
  if(k==K_LEFT||k=='h') mv_left();
  else if(k==K_RIGHT||k=='l') mv_right();
  else if(k==K_WORD_LEFT) word_back();
  else if(k==K_WORD_RIGHT) word_fwd();
  else if(k==K_UP||k=='k') mv_vert(-1);
  else if(k==K_DOWN||k=='j') mv_vert(1);
  else if(k==K_HOME) cx=0;
  else if(k==K_END){ cx=(int)ln().size(); if(mode==NORM && cx) cx--; }
  else if(k==K_PGUP) page(-1);
  else if(k==K_PGDN) page(1);
  if(cursors.size()<=1) sync_primary();
}

int process(){
  msg.clear();
  if(terminal_popup){
    pollfd fds[2]={{0,POLLIN,0},{terminal_fd,(short)(POLLIN|POLLHUP|POLLERR),0}};
    int ready;
    do { ready=poll(fds,2,-1); } while(ready<0&&errno==EINTR);
    if(ready<=0) return 0;
    if(fds[1].revents&(POLLIN|POLLHUP|POLLERR)){
      terminal_drain();
      if(terminal_popup && (fds[1].revents&(POLLHUP|POLLERR))){
        int status=0;
        pid_t done=terminal_pid>0?waitpid(terminal_pid,&status,WNOHANG):0;
        if(done==terminal_pid || terminal_fd<0){
          if(done==terminal_pid) terminal_pid=-1;
          terminal_close(); msg="shell exited"; return 0;
        }
      }
      if(!terminal_popup) return 0;
    }
    if(!(fds[0].revents&POLLIN)) return 0;
    int k=readk();
    if(!k) return 0;
    if(k==27){ terminal_close(); msg="terminal closed"; return 0; }
    terminal_send(k);
    return 0;
  }
  int k=readk();
  if(!k) return 0;
if(palette_open){
  if(k==27||k==' '||k=='q'){ palette_open=false; return 0; }
  palette_open=false;
  if(k=='f'){ open_picker(); return 0; }
  if(k=='v'){ create_split(true,""); return 0; }
  if(k=='s'){ create_split(false,""); return 0; }
  if(k=='c'){ cbuf="close"; colon(); return 0; }
  if(k=='h'||k=='?'){ open_help(); return 0; }
  if(k=='n'){ open_snippet_list(); return 0; }
  if(k=='m'){ if(custom_commands.count("make")) run_custom_command("make"); else msg="no configured :make command"; return 0; }
   if(k=='t'){ terminal_start(); return 0; }
  if(k=='w'){ save(); return 0; }
  return 0;
}
  if(picker_open){
    vector<string> matches=picker_matches();
    if(k==27){ picker_open=false; return 0; }
    if(k==K_UP||k==K_DOWN||k==14||k==16){
      int d=(k==K_UP||k==16)?-1:1;
      if(!matches.empty()) picker_selected=(picker_selected+d+(int)matches.size())%(int)matches.size();
      return 0;
    }
    if(k=='\n'){
      if(!matches.empty()){ load(matches[min(picker_selected,(int)matches.size()-1)]); load_snips(); msg="opened "+fname; }
      picker_open=false; return 0;
    }
    if(k==K_BACK||k==8){ if(!picker_query.empty()) picker_query.pop_back(); picker_selected=0; return 0; }
    if(k>=32&&k<127){ picker_query.push_back((char)k); picker_selected=0; return 0; }
    return 0;
  }
  if(k==K_CLICK){ click_to(); return 0; }
  if(k==K_WHEEL_UP){ scroll_lines(-8); return 0; }
  if(k==K_WHEEL_DOWN){ scroll_lines(8); return 0; }
  if(k==K_PASTE_BEGIN){
    string pasted=read_bracketed_paste();
    if(mode==CMD) paste_into_command(pasted);
    else paste_into_buffer(pasted);
    return 0;
  }
  if(k==27 && mode==NORM && !shell_popup.empty()){
    shell_popup.clear();
    return 0;
  }
  if(k==27 && mode==NORM && cursors.size()>1){ reset_cursors(); return 0; }
  if(k==14 && mode==NORM){ add_next_cursor(false); return 0; }
  if(k==24 && mode==NORM){ add_next_cursor(true); return 0; }
  if(k==22){
    string pasted=read_raw_paste();
    if(pasted.empty()){ msg="paste with Ctrl+Shift+V or Command+V"; return 0; }
    if(mode==CMD) paste_into_command(pasted);
    else paste_into_buffer(pasted);
    return 0;
  }
  if(mode==CMD){
    if(k==27){ mode=NORM; cbuf.clear(); }
    else if(k=='\n'){ int q=colon(); cbuf.clear(); mode=NORM; if(q) return 1; }
    else if(k==K_BACK||k==8){ if(!cbuf.empty()) cbuf.pop_back(); else mode=NORM; }
    else if(k==K_TAB){
      string t=first_tok(cbuf);
      vector<string> hits;
      for(int i=0;i<NHELP;i++)
        if(cmd_match(CHELP[i], t)) hits.push_back(CHELP[i].name);
      if(hits.size()==1) cbuf=hits[0];
      else if(hits.size()>1){
        string p=hits[0];
        for(size_t i=1;i<hits.size();i++){
          size_t n=0; while(n<p.size() && n<hits[i].size() && p[n]==hits[i][n]) n++;
          p.resize(n);
        }
        if(p.size()>t.size()) cbuf=p;
      }
    }
    else if(k>=32&&k<127) cbuf.push_back((char)k);
    return 0;
  }
  if(mode==INS){
    if(k==27){ mode=NORM; snip_end(); if(cx>0) cx--; clampx(); }
    else if(k==K_TAB){
      if(sess.idx>=0) snip_next(1);
      else if(!try_expand()) ins_tab();
    }
    else if(k==K_STAB){ if(sess.idx>=0) snip_next(-1); }
    else if(k=='\n'){ snip_end(); if(cursors.size()>1) multi_insert_text("\n"); else split(); }
    else if(k==K_BACK||k==8){
      if(cursors.size()>1){ multi_backspace(); return 0; }
      if(sess.idx>=0 && sess.sel){
        Stop& s=sess.st[sess.idx];
        if(s.len>0 && s.y==cy){
          if(s.x+s.len<=(int)ln().size()) ln().erase(s.x,s.len);
          adj_stops(s.y,s.x,-s.len); cx=s.x; s.len=0; sess.sel=0; dirty=1;
        } else bs();
      } else bs();
    }
    else if(k==K_DEL){ if(cursors.size()>1) multi_delete(); else del_char(); }
    else if(k==K_PGUP||k==K_PGDN||k==K_LEFT||k==K_RIGHT||k==K_WORD_LEFT||k==K_WORD_RIGHT||k==K_UP||k==K_DOWN||k==K_HOME||k==K_END) do_move_key(k);
    else if(k>=32&&k<127){
      if(cursors.size()>1) multi_insert((char)k);
      else { snip_type((char)k); try_auto(); }
    }
    return 0;
  }
if(scratch_buffer){
  if(k=='q'){ close_scratch(); return 0; }
  if(k=='i'||k=='a'||k=='I'||k=='A'||k=='o'||k=='O'||k=='x'||
     k=='D'||k=='d'||k==K_DEL){
    msg="read-only buffer (:q to close)";
    return 0;
  }
}
if(k==' ' && mode==NORM){ palette_open=true; return 0; }
  if(!pend.empty()){
    if(k>=32&&k<127) pend.push_back((char)k); else { pend.clear(); return 0; }
    if(pend=="da") return 0;
     if(pend=="dd"){ if(cursors.size()>1) multi_delete_line(); else del_line(); }
     else if(pend=="daw"){ if(cursors.size()>1) multi_word_delete(true); else daw(); }
     else if(pend=="dw"){ if(cursors.size()>1) multi_word_delete(false); else dw(); }
     else if(pend=="d$"){ if(cursors.size()>1){ for(auto c:cursors) if(c.y<(int)L.size()) L[c.y].erase(c.x); dirty=1; } else { ln().erase(cx); dirty=1; clampx(); } }
    else if(pend=="gg"){ cy=0; cx=0; }
    pend.clear();
    return 0;
  }
  if(k=='d'||k=='g'){ pend=(char)k; return 0; }
  if(k=='i') mode=INS;
  else if(k=='a'){ if(cx<(int)ln().size()) cx++; mode=INS; }
  else if(k=='I'){ cx=0; mode=INS; }
  else if(k=='A'){ cx=(int)ln().size(); mode=INS; }
  else if(k=='o'){ L.insert(L.begin()+cy+1,""); cy++; cx=0; mode=INS; dirty=1; }
  else if(k=='O'){ L.insert(L.begin()+cy,""); cx=0; mode=INS; dirty=1; }
  else if(k=='x'||k==K_DEL){ if(cursors.size()>1) multi_delete(); else del_char(); }
  else if(k=='D'){ if(cursors.size()>1){ for(auto c:cursors) if(c.y<(int)L.size()) L[c.y].erase(c.x); dirty=1; } else { ln().erase(cx); dirty=1; clampx(); } }
  else if(k=='0') cx=0;
  else if(k=='$'){ cx=(int)ln().size(); if(cx) cx--; }
  else if(k=='w') word_fwd();
  else if(k=='b') word_back();
  else if(k=='G'){ cy=nlines()-1; clampx(); }
  else if(k==':'){ mode=CMD; cbuf.clear(); }
  else if(k==27 && cursors.size()>1){ reset_cursors(); }
  else if(k==23){
    int c=readk();
    if(panes.size()==2 && ((split_vertical&&(c=='h'||c=='l'))||
                           (split_horizontal&&(c=='j'||c=='k')))){
      save_active_pane(); active_pane=1-active_pane; load_active_pane();
      cursors.clear(); reset_cursors(); msg="pane "+to_string(active_pane+1);
    }
  }
  else if(k==16) open_picker();
  else if(k==2) page(-1);
  else if(k==6) page(1);
  else if(k==21) page(-1);
  else if(k==4) page(1);
  else if(k==25) scroll_lines(-5);
  else if(k==5) scroll_lines(5);
  else if(k=='q'){ if(dirty) msg="unsaved — :wq or :q!"; else return 1; }
  else do_move_key(k);
  return 0;
}

int main(int argc,char** argv){
  load_cfg();
  if(argc>1) load(argv[1]); else { L.push_back(""); reset_cursors(); }
  panes.push_back(capture_pane());
  load_snips();
  atexit(ts_cleanup);
  atexit(terminal_close);
  raw_on(); mouse_on(); winsz();
  msg=": command   click to move   tab=snippet";
  while(1){ draw(); if(process()) break; }
  return 0;
}

