#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
#include <poll.h>
#include <termios.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include <sys/wait.h>
using namespace std;

enum {
  K_UP=1000, K_DOWN, K_LEFT, K_RIGHT, K_HOME, K_END, K_DEL,
  K_PGUP, K_PGDN, K_STAB, K_CLICK, K_WHEEL_UP, K_WHEEL_DOWN,
  K_PASTE_BEGIN, K_PASTE_END,
  K_TAB=9, K_BACK=127
};
enum Mode { NORM, INS, CMD };

struct termios orig;
vector<string> L;
int cx=0, cy=0, off=0, coff=0, rows=24, cols=80, dirty=0;
int tabstop=4, expandtab=0, esc_ms=8, scrolloff=2, mouse=1;
int click_sx=1, click_sy=1;
string fname, msg, cbuf, pend;
vector<string> shell_popup;
Mode mode=NORM;
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
string POPUP_BG="\x1b[48;2;38;58;43m";
string POPUP_TEXT="\x1b[38;2;173;194;174m";
string POPUP_SELECTED_BG="\x1b[48;2;54;82;59m";
string POPUP_SELECTED_TEXT="\x1b[38;2;220;231;216m";
string POPUP_FACE=POPUP_BG+POPUP_TEXT;
string POPUP_SELECTED_FACE=POPUP_SELECTED_BG+POPUP_SELECTED_TEXT;
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
  {"help","h","help [cmd]","list commands, or explain one","command completion appears as you type"},
};
static const int NHELP=(int)(sizeof(CHELP)/sizeof(CHELP[0]));

void die(const char* s){ perror(s); exit(1); }
void raw_off(){
  tcsetattr(0, TCSAFLUSH, &orig);
  const char* s="\x1b[?1000l\x1b[?1006l\x1b[?2004l\x1b[?7h\x1b[?25h\x1b[0m\x1b[?1049l";
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
  POPUP_FACE=POPUP_BG+POPUP_TEXT;
  POPUP_SELECTED_FACE=POPUP_SELECTED_BG+POPUP_SELECTED_TEXT;
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
  vector<string> paths={"nv.json"};
  if(const char* h=getenv("HOME")){
    paths.push_back(string(h)+"/.nv.json");
    paths.push_back(string(h)+"/.config/nv.json");
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
  msg="snippets "+to_string((int)snips.size());
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
  int wait = poll(&p,1,0)>0 ? 0 : esc_ms;
  int a=rd_wait(wait); if(a<0) return 27;
  if(a=='['){
    int b=rd_wait(wait<0?8:max(8,wait)); if(b<0) return 27;
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
      if(ch=='M' && (btn&32)==0 && (btn&3)==0){
        click_sx=max(1,x); click_sy=max(1,y); return K_CLICK;
      }
      return 0;
    }
    if(b>='0'&&b<='9'){
      int n=b-'0', d;
      while((d=rd_wait(8))>='0'&&d<='9') n=n*10+d-'0';
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
  int g=(int)to_string(max(1,nlines())).size()+3;
  if(click_sy>=rows) return;
  int row=off+(click_sy-1);
  if(row<0) row=0;
  if(row>=nlines()){ cy=nlines()-1; cx=(int)ln().size(); clampx(); return; }
  cy=row;
  int trx=click_sx-1-g;
  if(trx<0) trx=0;
  trx+=coff;
  cx=rx_to_cx(ln(),trx);
  clampx();
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
    if(!msg.empty()) right+="  "+msg;
  }
  if((int)left.size()+(int)right.size()+1>cols){
    int room=max(0,cols-(int)left.size()-1);
    if((int)right.size()>room) right=right.substr(right.size()-room);
  }
  o+=BG_BAR;
  o+="\x1b["+to_string(rows)+";1H";
  o+=FG_BRIGHT;
  o+=left;
  int spaces=max(1,cols-(int)left.size()-(int)right.size());
  o.append(spaces,' ');
  o+=right;
  o.append(max(0,cols-(int)left.size()-spaces-(int)right.size()),' ');
  o+=RESET;
}

void draw(){
  winsz();
  int h=max(1,rows-1), g=gw(), tw=max(1,cols-g-1);
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
      for(char ch: s){
        int w=vis_w(ch,col);
        for(int k=0;k<w;k++){
          if(col+k>=coff && col+k<coff+tw && used<cols){
            o+=(ch=='\t'?' ':ch);
            used++;
          }
        }
        col+=w;
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
  draw_completions(o);
  draw_status(o);
  int curx, cury;
  if(mode==CMD){ cury=rows; curx=min(cols,(int)cbuf.size()+2); }
  else { curx=g+1+(rx-coff); cury=cy-off+1; }
  o+="\x1b["+to_string(cury)+";"+to_string(curx)+"H\x1b[?7h\x1b[?25h";
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
    run_shell_command(rest);
    return 0;
  }
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
  if(t=="q"||t=="quit"){ if(dirty){ msg="No write since last change (:q! to force)"; return 0; } return 1; }
  if(t=="q!") return 1;
  if(t=="w"||t=="write"){ if(!rest.empty()) fname=rest; save(); return 0; }
  if(t=="wq"||t=="x"){ if(!rest.empty()) fname=rest; if(!save()) return 0; return 1; }
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
  else if(k==K_UP||k=='k') mv_vert(-1);
  else if(k==K_DOWN||k=='j') mv_vert(1);
  else if(k==K_HOME) cx=0;
  else if(k==K_END){ cx=(int)ln().size(); if(mode==NORM && cx) cx--; }
  else if(k==K_PGUP) page(-1);
  else if(k==K_PGDN) page(1);
}

int process(){
  msg.clear();
  int k=readk();
  if(!k) return 0;
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
    else if(k=='\n'){ snip_end(); split(); }
    else if(k==K_BACK||k==8){
      if(sess.idx>=0 && sess.sel){
        Stop& s=sess.st[sess.idx];
        if(s.len>0 && s.y==cy){
          if(s.x+s.len<=(int)ln().size()) ln().erase(s.x,s.len);
          adj_stops(s.y,s.x,-s.len); cx=s.x; s.len=0; sess.sel=0; dirty=1;
        } else bs();
      } else bs();
    }
    else if(k==K_DEL) del_char();
    else if(k==K_PGUP||k==K_PGDN||k==K_LEFT||k==K_RIGHT||k==K_UP||k==K_DOWN||k==K_HOME||k==K_END) do_move_key(k);
    else if(k>=32&&k<127){ snip_type((char)k); try_auto(); }
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
  if(!pend.empty()){
    if(k>=32&&k<127) pend.push_back((char)k); else { pend.clear(); return 0; }
    if(pend=="da") return 0;
    if(pend=="dd") del_line();
    else if(pend=="daw") daw();
    else if(pend=="dw") dw();
    else if(pend=="d$"){ ln().erase(cx); dirty=1; clampx(); }
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
  else if(k=='x'||k==K_DEL) del_char();
  else if(k=='D'){ ln().erase(cx); dirty=1; clampx(); }
  else if(k=='0') cx=0;
  else if(k=='$'){ cx=(int)ln().size(); if(cx) cx--; }
  else if(k=='w') word_fwd();
  else if(k=='b') word_back();
  else if(k=='G'){ cy=nlines()-1; clampx(); }
  else if(k==':'){ mode=CMD; cbuf.clear(); }
  else if(k==19) save();
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
  if(argc>1) load(argv[1]); else L.push_back("");
  load_snips();
  raw_on(); mouse_on(); winsz();
  msg=": command   click to move   tab=snippet";
  while(1){ draw(); if(process()) break; }
  return 0;
}
