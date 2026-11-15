// Roster Room (Modern; Settings → Season → Roster Room): a page in your browser, served by the app on this
// computer only (127.0.0.1), for setting any team's default crew: who plays each of the 15 slots, over the weekly
// lineup. LOCK IN saves the crews (the same crews.json Pick Your Crew uses; crew.cpp) and closes the room; they're
// laid over the lineup at the next game (content_pack_tp.cpp) and go into an exported ROM too.
// A console or a phone has no browser of its own for this (include/blitz_platform.h: no local browser): there the room
// is served on the local network for a phone or a computer to open (the address is shown), and closes at LOCK IN
// like everywhere else.
#if BLITZ_MODERN
#include "blitz_platform.h"
#include "../main/net_compat.h"
#define SDL_MAIN_HANDLED
#include "SDL2/SDL.h"

#include <atomic>
#include <algorithm>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <thread>
#include <vector>

#include <fstream>
#include <iterator>

#include "json/json.hpp"
#include "util/file.h"
#include "rosters_live.h"

namespace blitz {
    bool blitz_portrait(int skin, const std::string& franchise, std::vector<char>& rgba, uint32_t& w, uint32_t& h);  // portraits.cpp
    std::vector<std::string> team_franchises();  // content_pack_tp.cpp
    std::string team_display_name(const std::string& franchise);  // content_pack_tp.cpp
    std::string crews_json();                           // crew.cpp
    int crews_lock_in(const std::string& body);
    void post_status(const std::string& msg);           // notices.cpp
}

namespace blitz {
    std::atomic<bool> crews_changed{false};  // team select re-applies the lineup and crews (uniform_picker.cpp)
}

namespace {
    using json = nlohmann::json;
    std::atomic<int> listen_fd{-1};
    std::atomic<int> port{0};
    std::atomic<bool> running{false};

    const char* page = R"HTML(<!doctype html>
<html lang="en"><head><meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>Roster Room</title>
<style>
@font-face{font-family:"Barlow Condensed";font-style:italic;font-weight:800;src:url(/a/BarlowCondensed-ExtraBoldItalic.ttf)}
@font-face{font-family:"Barlow Condensed";font-style:italic;font-weight:700;src:url(/a/BarlowCondensed-BoldItalic.ttf)}
@font-face{font-family:"Inter";font-weight:100 900;src:url(/a/InterVariable.ttf)}
:root{--ground:#020712;--navy:#070F22;--deep:#0D1A3A;--glass:rgba(6,18,44,.92);--rim:rgba(143,239,245,.7);--rim-soft:rgba(143,239,245,.24);
--text:#F2F2F2;--dim:#B8C4DA;--yellow:#F6D400;--yellow-hi:#FFE14A;--red:#E0483E;--cyan:#8FEFF5;
--display:"Barlow Condensed","Avenir Next Condensed",sans-serif;--body:"Inter",system-ui,-apple-system,sans-serif}
*{box-sizing:border-box}
html,body{margin:0;min-height:100%;color:var(--text);font:500 15px/1.35 var(--body)}
body{background:radial-gradient(80% 60% at 100% 0%,rgba(63,10,95,.55),transparent 70%),radial-gradient(70% 60% at 0% 100%,rgba(13,26,58,.9),transparent 70%),var(--ground)}
.cap{font-family:var(--display);font-style:italic;font-weight:800;text-transform:uppercase;letter-spacing:.02em}
header{position:sticky;top:0;z-index:5;display:flex;align-items:center;gap:18px;padding:12px 24px 14px;
 background:linear-gradient(180deg,rgba(2,7,18,.97),rgba(2,7,18,.85));backdrop-filter:blur(6px)}
header::after{content:"";position:absolute;left:0;right:0;bottom:0;height:4px;background:url(/a/ui/rule_flame.png) center/100% 100% no-repeat}
header img{height:44px}
h1{white-space:nowrap;margin:0;font:italic 800 40px/1 var(--display);color:var(--yellow);text-transform:uppercase;letter-spacing:.02em;text-shadow:0 2px 0 #000}
.sub{color:var(--dim);font-size:13px;max-width:52ch}
.grow{flex:1}
button{white-space:nowrap;font:italic 800 18px/1 var(--display);text-transform:uppercase;letter-spacing:.04em;cursor:pointer;border-radius:3px;
 border:2px solid var(--rim-soft);background:var(--deep);color:var(--text);padding:10px 16px}
button:hover,button:focus-visible{border-color:var(--yellow);outline:none;color:var(--yellow-hi)}
#lock{background:linear-gradient(180deg,var(--yellow-hi),var(--yellow));color:#101018;border-color:#fff3a0;font-size:26px;padding:12px 30px;
 box-shadow:0 0 0 2px rgba(0,0,0,.5),0 0 24px rgba(246,212,0,.35)}
#lock:hover{color:#000;filter:brightness(1.08)}
main{display:grid;grid-template-columns:240px 1fr;gap:20px;padding:20px 24px 40px}
.frame{position:relative;background:linear-gradient(rgba(6,18,44,.82),rgba(6,18,44,.82)),url(/a/ui/grid_tile.png);background-color:var(--glass);border:2px solid var(--rim-soft);border-radius:4px}
.frame::before,.frame::after{content:"";position:absolute;width:22px;height:22px;background-size:contain;background-repeat:no-repeat;pointer-events:none}
.frame::before{left:-6px;top:-6px;background-image:url(/a/ui/chrome_corner_tl.png)}
.frame::after{right:-6px;bottom:-6px;background-image:url(/a/ui/chrome_corner_br.png)}
nav{display:grid;gap:4px;align-content:start;max-height:calc(100vh - 120px);overflow:auto;padding:10px}
nav button{text-align:left;display:flex;justify-content:space-between;font-size:17px;border-color:transparent;background:transparent;padding:8px 10px}
nav button.on{border-color:var(--yellow);background:rgba(246,212,0,.08);color:var(--yellow)}
nav .dot{color:var(--yellow);font-style:normal}
section{padding:18px 22px 24px}
section h2{margin:0 0 4px;font:italic 800 38px/1 var(--display);color:var(--yellow);text-transform:uppercase}
.groups{display:flex;flex-wrap:wrap;gap:6px 28px}.g{flex:0 0 auto}
.group{margin:18px 0 8px;color:var(--cyan);font:800 12px/1 var(--body);letter-spacing:.16em;text-transform:uppercase}
.slots{display:flex;flex-wrap:wrap;gap:10px}
.slot{width:132px;background:var(--navy);border:2px solid var(--rim-soft);border-radius:4px;padding:7px;text-align:center;cursor:pointer;transition:border-color .1s}
.slot:hover{border-color:var(--rim)}
.slot.set{border-color:var(--yellow);box-shadow:inset 0 0 0 1px rgba(246,212,0,.3)}
.slot img,.pick img{width:100px;height:73px;object-fit:cover;background:#18284e;border-radius:3px;display:block;margin:0 auto 5px}
.slot .pos{font:800 11px/1 var(--body);color:var(--cyan);letter-spacing:.12em}
.slot .nm{font:italic 800 20px/1.1 var(--display);text-transform:uppercase;white-space:nowrap;overflow:hidden;text-overflow:ellipsis;margin-top:3px}
.slot .tag{font-size:11px;color:var(--dim);letter-spacing:.08em;text-transform:uppercase}
.slot .tag.set{color:var(--yellow)}
#picker{position:fixed;inset:0;background:rgba(2,7,18,.82);display:none;align-items:center;justify-content:center;z-index:9}
#picker .box{padding:18px 20px;max-width:960px;max-height:86vh;overflow:auto}
#ptitle{font:italic 800 28px/1 var(--display);color:var(--yellow);text-transform:uppercase}
#picker .list{display:flex;flex-wrap:wrap;gap:10px;margin-top:12px}
.pick{width:124px;padding:7px;border:2px solid var(--rim-soft);border-radius:4px;text-align:center;cursor:pointer;background:var(--navy)}
.pick:hover{border-color:var(--rim)}.pick.cur{border-color:var(--yellow)}
.pick .nm{font:italic 800 18px/1.1 var(--display);text-transform:uppercase;white-space:nowrap;overflow:hidden;text-overflow:ellipsis}
.out{color:var(--red);font-weight:800}
#reset{margin-top:22px;font-size:16px}
#done{position:fixed;inset:0;background:var(--ground);display:none;align-items:center;justify-content:center;flex-direction:column;gap:14px;text-align:center;z-index:10}
#done img{height:80px}
#done h2{font:italic 800 72px/1 var(--display);color:var(--yellow);margin:0;text-transform:uppercase}
@media (max-width:720px){main{grid-template-columns:1fr}nav{grid-template-columns:repeat(2,1fr);max-height:none}header{flex-wrap:wrap}}
</style></head><body>
<header><img src="/a/ui/logo27_lockup.png" alt=""><h1>Roster Room</h1><div class="sub" id="sub">Loading…</div><div class="grow"></div>
<button id="cancel">Close without saving</button><button id="lock">Lock in</button></header>
<main><nav id="teams" class="frame"></nav><section id="team" class="frame"></section></main>
<div id="picker"><div class="box frame"><div style="display:flex;gap:10px;align-items:center"><span id="ptitle"></span><div class="grow"></div><button id="pclose">Back</button></div><div class="list" id="plist"></div></div></div>
<div id="done"><img src="/a/ui/logo27_lockup.png" alt=""><h2 id="dtitle">Locked in</h2><div class="sub" id="dtext">Back to the game: your crews play from the next game. You can close this tab.</div></div>
<script>
let NAMES={};
const SLOTS=[["Offensive line",[0,1,2],"OL"],["Quarterback",[3],"QB"],["Running back",[4],"RB"],["Receivers",[5,6],"REC"],["Defensive line",[7,8,9],"DL"],["Back seven",[10,11,12,13],"DEF"],["Kicker",[14],"K"]];
const POS={OL:["OT","T","G","OG","C","OC","OL"],QB:["QB"],RB:["RB","FB","HB"],REC:["WR","TE","RB","FB","HB"],DL:["DE","DT","NT","DL","EDGE","OLB"],DEF:["LB","ILB","MLB","OLB","CB","S","SS","FS","DB"],K:["PK","K"]};
const face=id=>/^\d+$/.test(id)?`https://a.espncdn.com/combiner/i?img=/i/headshots/nfl/players/full/${id}.png&w=96&h=70&format=jpg`:`/p/${encodeURIComponent(id)}`;
let D=null,cur=null,crews={},touched=new Set();
const $=id=>document.getElementById(id);
function el(tag,cls,text){const e=document.createElement(tag);if(cls)e.className=cls;if(text!==undefined)e.textContent=text;return e}
function player(f,id){return (D.players[f]||[]).find(p=>p.id===id)}
function rank(p){const m=/(\d+)$/.exec(p.depth||"");return m?+m[1]:99}
function teams(){const n=$("teams");n.textContent="";for(const f of D.teams){const b=el("button",f===cur?"on":"");b.append(el("span","",NAMES[f]||f));if(crews[f]&&Object.keys(crews[f]).length)b.append(el("span","dot","●"));b.onclick=()=>{cur=f;render()};n.append(b)}}
function render(){teams();const s=$("team");s.textContent="";s.append(el("h2","",NAMES[cur]||cur));
 s.append(el("div","sub","Click a slot to choose who plays there. Yellow slots are your picks; the rest follow this week's lineup ("+D.week+")."));
 const c=crews[cur]||{};
 const G=el("div","groups");s.append(G);for(const [label,slots,grp] of SLOTS){const g=el("div","g");G.append(g);g.append(el("div","group",label));const row=el("div","slots");
  for(const sl of slots){const id=c[sl]||D.live[cur]?.[sl]||"";const p=id?player(cur,id):null;const d=el("div","slot"+(c[sl]?" set":""));
   const img=el("img");img.alt="";if(p)img.src=face(p.id);d.append(img);d.append(el("div","pos",grp+(slots.length>1?" "+(slots.indexOf(sl)+1):"")));
   d.append(el("div","nm",p?`#${p.number} ${p.last.toUpperCase()}`:"—"));const t=el("div","tag"+(c[sl]?" set":""),c[sl]?"your pick":"this week");d.append(t);
   d.onclick=()=>pick(sl,grp);row.append(d)}
  g.append(row)}
 const r=el("button","","Reset "+(NAMES[cur]||cur)+" to this week's");r.id="reset";r.onclick=()=>{crews[cur]={};touched.add(cur);render()};s.append(r)}
function pick(sl,grp){const c=crews[cur]||(crews[cur]={});$("ptitle").textContent=(NAMES[cur]||cur)+" · "+grp;const L=$("plist");L.textContent="";
 const live=el("div","pick"+(c[sl]?"":" cur"));live.append(el("img"));live.append(el("div","","This week's"));live.onclick=()=>{delete c[sl];touched.add(cur);close();render()};L.append(live);
 const ps=(D.players[cur]||[]).filter(p=>POS[grp].includes(p.pos)).sort((a,b)=>rank(a)-rank(b));
 for(const p of ps){const d=el("div","pick"+(c[sl]===p.id?" cur":""));const img=el("img");img.src=face(p.id);img.alt="";d.append(img);
  d.append(el("div","nm",`#${p.number} ${p.last.toUpperCase()}`));const st=p.status==="reserve"?"IR":p.status==="out"?"OUT":(p.depth||"").toUpperCase();d.append(el("div",p.status==="reserve"||p.status==="out"?"out":"sub",st+" · "+p.pos));
  d.onclick=()=>{for(const k of Object.keys(c))if(c[k]===p.id&&+k!==sl)delete c[k];c[sl]=p.id;touched.add(cur);close();render()};L.append(d)}
 $("picker").style.display="flex"}
function close(){$("picker").style.display="none"}
$("pclose").onclick=close;
$("lock").onclick=async()=>{const body={};for(const f of touched)body[f]=crews[f]||{};
 const r=await fetch("/lockin",{method:"POST",body:JSON.stringify(body)}).catch(()=>null);
 $("dtitle").textContent=r&&r.ok?"Locked in":"Couldn't save";if(!(r&&r.ok))$("dtext").textContent="The app didn't answer. Is it still open?";$("done").style.display="flex"};
$("cancel").onclick=async()=>{await fetch("/close",{method:"POST"}).catch(()=>null);$("dtitle").textContent="Closed";$("dtext").textContent="Nothing was changed. You can close this tab.";$("done").style.display="flex"};
fetch("/data").then(r=>r.json()).then(d=>{D=d;NAMES=d.names||{};crews=d.crews||{};D.teams.sort((a,b)=>(NAMES[a]||a).localeCompare(NAMES[b]||b));for(const k of Object.keys(crews))if(k.startsWith("PRO_"))delete crews[k];
 cur=D.teams[0];$("sub").textContent="Default crews for every team · lineups: "+D.week;render()});
</script></body></html>)HTML";

    std::string data_json() {
        json out;
        out["teams"] = json::array();
        out["players"] = json::object();
        out["live"] = json::object();
        out["names"] = json::object();  // the game's own team names (no club's name is written into the app)
        for (const std::string& f : blitz::team_franchises()) {
            out["teams"].push_back(f);
            out["names"][f] = blitz::team_display_name(f);
            json list = json::array();
            for (const auto& p : blitz::roster_of(f))
                list.push_back({{"id", p.id}, {"first", p.first}, {"last", p.last}, {"number", p.number}, {"pos", p.pos},
                                {"depth", p.depth}, {"status", p.status}});
            out["players"][f] = list;
            auto ids = blitz::lineup_ids(f);
            out["live"][f] = json(std::vector<std::string>(ids.begin(), ids.end()));
        }
        out["crews"] = json::parse(blitz::crews_json(), nullptr, false);
        auto lineup = blitz::lineup_now(-1);
        out["week"] = lineup ? lineup->label : "as packed";
        return out.dump();
    }

    void send_all(int fd, const std::string& s) {
        size_t sent = 0;
        while (sent < s.size()) {
            ssize_t n = blitz::net::send_some(fd, s.data() + sent, s.size() - sent);
            if (n <= 0) return;
            sent += (size_t)n;
        }
    }

    void reply(int fd, const char* type, const std::string& body, int code = 200) {
        send_all(fd, std::string("HTTP/1.1 ") + (code == 200 ? "200 OK" : "400 Bad Request") + "\r\nContent-Type: " + type +
                         "\r\nCache-Control: no-store\r\nContent-Length: " + std::to_string(body.size()) + "\r\nConnection: close\r\n\r\n" + body);
    }

    // A player's Blitz portrait (portraits.cpp) as a 24-bit BMP on the cards' navy, for GET /p/<id>: the page shows the
    // game's own faces, as the app does, rather than photos. Empty for an unknown player or without a ROM.
    std::string portrait_bmp(const std::string& id) {
        auto p = blitz::roster_player(id);
        if (!p) return {};
        std::vector<char> rgba;
        uint32_t w = 0, h = 0;
        if (!blitz::blitz_portrait(p->skin, p->team, rgba, w, h)) return {};
        uint32_t row = (w * 3 + 3) & ~3u, size = 54 + row * h;
        std::string out(size, '\0');
        auto put32 = [&](size_t at, uint32_t v) { for (int i = 0; i < 4; i++) out[at + (size_t)i] = (char)(v >> (8 * i)); };
        out[0] = 'B';
        out[1] = 'M';
        put32(2, size);
        put32(10, 54);
        put32(14, 40);
        put32(18, w);
        put32(22, h);
        out[26] = 1;
        out[28] = 24;
        const uint8_t navy[3] = {24, 40, 78};  // .slot img's background
        for (uint32_t y = 0; y < h; y++) {
            for (uint32_t x = 0; x < w; x++) {
                const uint8_t* src = (const uint8_t*)rgba.data() + ((h - 1 - y) * w + x) * 4;  // bottom row first
                float a = src[3] / 255.0f;
                for (int c = 0; c < 3; c++) {  // BGR
                    out[54 + y * row + x * 3 + (uint32_t)c] = (char)(uint8_t)(src[2 - c] * a + navy[2 - c] * (1 - a));
                }
            }
        }
        return out;
    }

    void stop() {
        running = false;
        int fd = listen_fd.exchange(-1);
        if (fd >= 0) {
            blitz::net::shutdown_socket(fd);
            blitz::net::close_socket(fd);
        }
    }

    // One request a connection: GET / (the page), GET /data, GET /p/<id> (a portrait), POST /lockin, POST /close.
    void handle(int fd) {
        std::string req;
        char buf[8192];
        size_t header_end = std::string::npos, want = 0;
        while (req.size() < (4u << 20)) {
            ssize_t n = blitz::net::recv_some(fd, buf, sizeof(buf));
            if (n <= 0) break;
            req.append(buf, (size_t)n);
            if (header_end == std::string::npos && (header_end = req.find("\r\n\r\n")) != std::string::npos) {
                size_t cl = req.find("Content-Length:");
                if (cl == std::string::npos) cl = req.find("content-length:");
                want = cl != std::string::npos && cl < header_end ? (size_t)std::max(0, std::atoi(req.c_str() + cl + 15)) : 0;
            }
            if (header_end != std::string::npos && req.size() >= header_end + 4 + want) break;
        }
        std::string line = req.substr(0, req.find("\r\n"));
        std::string body = header_end != std::string::npos ? req.substr(header_end + 4) : "";
        // Its own thread: nothing may escape (a player file string that isn't UTF-8 makes the JSON dump throw).
        try {
            if (line.rfind("GET / ", 0) == 0) reply(fd, "text/html; charset=utf-8", page);
            else if (line.rfind("GET /a/", 0) == 0) {  // the app's own fonts and Blitz '27 art (Resources/assets)
                std::string path = line.substr(7, line.find(' ', 7) - 7);
                bool ok = path.find("..") == std::string::npos && (path.rfind("ui/", 0) == 0 || path.find(".ttf") != std::string::npos);
                std::ifstream in{recompui::file::get_program_path() / "assets" / path, std::ios::binary};
                if (!ok || !in) reply(fd, "text/plain", "not found", 400);
                else {
                    std::string data{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
                    const char* type = path.find(".png") != std::string::npos ? "image/png" : "font/ttf";
                    send_all(fd, std::string("HTTP/1.1 200 OK\r\nContent-Type: ") + type + "\r\nCache-Control: max-age=3600\r\nContent-Length: " +
                                     std::to_string(data.size()) + "\r\nConnection: close\r\n\r\n" + data);
                }
            }
            else if (line.rfind("GET /data", 0) == 0) reply(fd, "application/json", data_json());
            else if (line.rfind("GET /p/", 0) == 0) {  // a player's portrait
                std::string id = line.substr(7, line.find(' ', 7) - 7);
                for (size_t at; (at = id.find("%3A")) != std::string::npos;) id.replace(at, 3, ":");  // nfl:<gsis id>
                bool ok = !id.empty() && std::all_of(id.begin(), id.end(), [](char c) { return std::isalnum((unsigned char)c) || c == ':' || c == '-'; });
                std::string bmp = ok ? portrait_bmp(id) : std::string();
                if (bmp.empty()) reply(fd, "text/plain", "not found", 400);
                else send_all(fd, "HTTP/1.1 200 OK\r\nContent-Type: image/bmp\r\nCache-Control: max-age=3600\r\nContent-Length: " +
                                      std::to_string(bmp.size()) + "\r\nConnection: close\r\n\r\n" + bmp);
            }
            else if (line.rfind("POST /lockin", 0) == 0) {
                int n = blitz::crews_lock_in(body);
                reply(fd, "application/json", n >= 0 ? "{\"ok\":true}" : "{\"ok\":false}", n >= 0 ? 200 : 400);
                if (n >= 0) {
                    blitz::crews_changed = true;
                    blitz::post_status("Roster Room: crews locked in for the next game");
                    fprintf(stderr, "[roster-room] locked in (%d teams changed)\n", n);
                    blitz::net::close_socket(fd);
                    stop();
                    return;
                }
            }
            else if (line.rfind("POST /close", 0) == 0) {
                reply(fd, "application/json", "{\"ok\":true}");
                blitz::net::close_socket(fd);
                stop();
                return;
            }
            else reply(fd, "text/plain", "not found", 400);
        }
        catch (const std::exception& e) {
            fprintf(stderr, "[roster-room] %s: %s\n", line.c_str(), e.what());
            reply(fd, "text/plain", "error", 500);
        }
        blitz::net::close_socket(fd);
    }
}

namespace blitz {
    // Settings → Season → Roster Room: starts the room (once) and opens it in the browser.
    void roster_room_open() {
        if (!running) {
            int fd = blitz::net::open_tcp();
            if (fd < 0) return;
            blitz::net::harden(fd);
            blitz::net::reuse_port(fd);
            int p = 0;
            for (int tryp = 8780; tryp < 8790 && !p; tryp++) {
                sockaddr_in addr{};
                addr.sin_family = AF_INET;
#if !BLITZ_HAS_LOCAL_BROWSER
                addr.sin_addr.s_addr = htonl(INADDR_ANY);  // a phone or computer on the network opens it
#else
                addr.sin_addr.s_addr = htonl(INADDR_LOOPBACK);  // this computer only
#endif
                addr.sin_port = htons((uint16_t)tryp);
                if (::bind(fd, (sockaddr*)&addr, sizeof(addr)) == 0) p = tryp;
            }
            if (!p || ::listen(fd, 8) != 0) {
                blitz::net::close_socket(fd);
                post_status("Roster Room couldn't start");
                return;
            }
            listen_fd = fd;
            port = p;
            running = true;
            std::thread([] {
                while (running) {
                    int lfd = listen_fd.load();
                    if (lfd < 0) break;
                    int c = blitz::net::accept_one(lfd);
                    if (c < 0) continue;
                    blitz::net::set_timeouts(c, 10000);  // a stalled browser can't hold the room
                    handle(c);
                }
            }).detach();
            fprintf(stderr, "[roster-room] http://127.0.0.1:%d/\n", p);
        }
#if !BLITZ_HAS_LOCAL_BROWSER
        // A console, or a phone that would suspend the game (and its server) to show a browser: another device opens it.
        std::string ip = blitz::net::local_address();
        if (ip.empty()) post_status("Roster Room: connect to your network first");
        else post_status("Roster Room: open http://" + ip + ":" + std::to_string(port.load()) + " on a phone or computer");
#else
        std::string url = "http://127.0.0.1:" + std::to_string(port.load()) + "/";
#ifdef BLITZ_MACOS
        if (getenv("BLITZ_NO_BROWSER") == nullptr) std::system(("open " + url).c_str());
#else
        if (getenv("BLITZ_NO_BROWSER") == nullptr) SDL_OpenURL(url.c_str());
#endif
        post_status("Roster Room: open in your browser");
#endif
    }

    int roster_room_port() { return running ? port.load() : 0; }
}
#else
#include <atomic>
#include <string>
namespace blitz {
    void roster_room_open() {}  // Classic has no Roster Room
    std::atomic<bool> crews_changed{false};
}
#endif
