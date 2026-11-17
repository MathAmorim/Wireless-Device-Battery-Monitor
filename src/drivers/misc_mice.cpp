// Smaller mouse brands, each with a community-documented battery protocol:
//  * Motospeed X6 (Realtek VID 0BDA, receivers FFE0 / FFF2, interface 4): output report B5 [06],
//    reply report B4, byte 20 = level (+128 while charging). The cable (FFF1) reports no battery.
//    From github.com/Jacobz888/LGSTrayBattery-MotospeedX6.
//  * Xenta receivers (Delux M900 Pro, Attack Shark X6 / R1 / X11 / X3 Max / V3 Pro; VID 1D57): the receiver
//    broadcasts input report 03 on usage page 000A every ~2 s: [03][model][40][state][percent],
//    state 2 = full, 3 = charging (percent frozen). From github.com/AeonArcHQ/omarchy-delux-m900.
//  * Ajazz AJ179 family (VID 249A receiver, 248A cable, interface 2): unnumbered 32-byte report,
//    command 10 (device info, answered by the receiver): reply [10][00 = ok]..., [4..7] model id,
//    [12] charging, [13] percent, [14] mouse connected (receiver), [31] = sum([4..30]).
//    From github.com/Rockeyxx/aj179-linux (docs/PROTOCOL.md).
//  * AJAZZ AJ-series mice on the SONiX vendor id 3151 (AJ159 PRO / APEX and siblings). The dongle
//    exposes a 65-byte feature collection on usage page FFFF / usage 02. A SET_FEATURE status poll
//    (report 0x00, opcode 0xF7, zero payload) brings the 2.4 GHz telemetry link up; ~30 ms later
//    GET_FEATURE report 0x05 carries the charge in byte 3 (0..100). A frame with a non-zero byte 1
//    or 2 is transient reconnect garbage and is rejected.
//    From github.com/Aiacos/ajazz-control-center (docs/protocols/mouse/aj_series.md).
#include "drivers.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>

namespace {

template <class Match>
bool ReadUntil(HidDev& d, std::vector<BYTE>& r, int timeoutMs, Match match) {
    ULONGLONG end = GetTickCount64() + timeoutMs;
    while ((LONGLONG)end - (LONGLONG)GetTickCount64() > 0) {
        int left = (int)((LONGLONG)end - (LONGLONG)GetTickCount64());
        if (d.Read(r, left) < 0) return false;
        if (match(r)) return true;
    }
    return false;
}

void PollMotospeed(PollContext& ctx) {
    for (USHORT pid : { (USHORT)0xFFE0, (USHORT)0xFFF2, (USHORT)0xFFF1 }) {
        if (ctx.IsClaimed(0x0BDA, pid)) continue;
        for (const HidColl* c : ctx.Find(0x0BDA, pid, [](const HidColl& c) { return c.iface == 4 && c.outLen() >= 21; })) {
            DeviceStatus s;
            s.key = L"motospeed:" + Hex4(pid);
            s.name = L"Motospeed X6";
            s.kind = DeviceKind::Mouse;
            if (pid == 0xFFF1) {  // plugged in: charging, no battery reading over the cable
                s.via = L"USB cable";
                s.charging = true;
                ctx.Add(s, 0x0BDA, pid);
                break;
            }
            HidDev d;
            if (!d.Open(*c)) continue;
            d.Flush();
            if (!d.Write({ 0xB5, 0x06 })) continue;
            std::vector<BYTE> r;
            if (!ReadUntil(d, r, 1500, [](const std::vector<BYTE>& r) { return r.size() > 20 && r[0] == 0xB4; })) continue;
            int raw = r[20];
            s.via = L"2.4 GHz receiver";
            s.charging = raw >= 128;
            s.percent = Clamp100(s.charging ? raw - 128 : raw);
            ctx.Add(s, 0x0BDA, pid);
            break;
        }
    }
}

// Xenta receiver (1D57:FA60, FA65; cable FA73 / FA55 / 2155 / FA61 / FA71..FA7D), used by Delux and Attack Shark mice.
// The receiver broadcasts [03][model][40][state][level] every ~2 s; nothing is sent to it. The handle
// stays open between polls so these frames queue up.
struct XentaModel { BYTE code; const wchar_t* name; bool tens; };
const XentaModel kXentaModels[] = {
    { 0x34, L"Delux M900 Pro 8K", false },
    { 0x3D, L"Delux M900 Pro", false },
    { 0x37, L"Delux M900", false },
    { 0x21, L"Delux M600", false },
    { 0x30, L"Delux M600 Pro", false },
    { 0x32, L"Delux M600 Pro 8K", false },
    { 0x38, L"Delux M800 Pro 3395", false },
    { 0x39, L"Delux M800 Pro 3311", false },
    { 0x3A, L"Delux M800 Pro+", false },
    { 0x3F, L"Delux M600 3325", false },
    { 0x29, L"Delux M500 Pro", false },
    { 0x50, L"Delux M300 Pro", false },
    { 0x56, L"Delux M700 Pro", false },
    { 0x55, L"Attack Shark X11", false },
    { 0x4E, L"Attack Shark X3 Max", false },
    { 0xE5, L"Attack Shark V3 Pro", false },
    { 0x10, L"Attack Shark X6 / R1", true },  // level reported in tens (0-10)
};

bool IsXentaCablePid(USHORT pid) {
    switch (pid) {
        case 0xFA73: case 0xFA55: case 0x2155: case 0xFA61:
        case 0xFA71: case 0xFA72: case 0xFA74: case 0xFA75:
        case 0xFA76: case 0xFA77: case 0xFA78: case 0xFA79:
        case 0xFA7A: case 0xFA7B: case 0xFA7C: case 0xFA7D:
            return true;
        default:
            return false;
    }
}

const wchar_t* DefaultXentaNameByPid(USHORT pid) {
    switch (pid) {
        case 0xFA65: case 0xFA73: return L"Delux M900 Pro 8K";
        case 0xFA72: return L"Delux M600 Pro 8K";
        case 0xFA74: return L"Delux M900";
        case 0xFA75: return L"Delux M800 Pro 3395";
        case 0xFA76: return L"Delux M800 Pro 3311";
        case 0xFA77: return L"Delux M800 Pro+";
        case 0xFA78: return L"Delux M800 Pro 3370";
        default: return L"Delux M900 Pro";
    }
}

struct XentaListener {
    std::unique_ptr<HidDev> dev;
    std::vector<BYTE> last, prev;  // the two most recent battery frames
    ULONGLONG lastFrameAt = 0;
    ULONGLONG prevFrameAt = 0;
};
std::map<std::wstring, XentaListener> g_xenta;  // by collection path
std::wstring g_lastXentaName;
int g_lastXentaPct = -1;

void PollXenta(PollContext& ctx) {
    const USHORT pids[] = {
        0xFA73, 0xFA55, 0x2155, 0xFA61, 0xFA71, 0xFA72, 0xFA74, 0xFA75,
        0xFA76, 0xFA77, 0xFA78, 0xFA79, 0xFA7A, 0xFA7B, 0xFA7C, 0xFA7D,
        0xFA65, 0xFA60
    };
    std::set<std::wstring> present;
    bool cablePresent = false;
    USHORT cablePid = 0;
    bool receiverPresent = false;
    USHORT receiverPid = 0;

    for (USHORT pid : pids) {
        if (ctx.IsClaimed(0x1D57, pid)) continue;
        auto allForPid = ctx.Find(0x1D57, pid, [](const HidColl&) { return true; });
        if (allForPid.empty()) continue;
        if (IsXentaCablePid(pid)) {
            cablePresent = true;
            if (!cablePid) cablePid = pid;
        } else {
            receiverPresent = true;
            if (!receiverPid) receiverPid = pid;
        }
    }

    if (!cablePresent && !receiverPresent) {
        g_xenta.clear();
        return;
    }

    // Read any queued frames from all present 000A collections (both cable and receiver).
    const XentaListener* bestListener = nullptr;
    USHORT activePid = cablePresent ? cablePid : receiverPid;

    for (USHORT pid : pids) {
        if (ctx.IsClaimed(0x1D57, pid)) continue;
        for (const HidColl* c : ctx.Find(0x1D57, pid, [](const HidColl& c) { return c.page() == 0x000A && c.inLen() >= 5; })) {
            present.insert(c->path);
            XentaListener& l = g_xenta[c->path];
            bool fresh = !l.dev;
            if (fresh) {
                l.dev = std::make_unique<HidDev>();
                if (!l.dev->Open(*c)) { g_xenta.erase(c->path); continue; }
            }
            // Drain everything queued since the last poll; on first open wait up to 500ms so we don't freeze if asleep.
            ULONGLONG until = GetTickCount64() + (fresh ? 500 : 0);
            std::vector<BYTE> r;
            int got = 0;
            while (true) {
                int wait = (fresh && got == 0) ? (int)std::max<LONGLONG>(1, (LONGLONG)until - (LONGLONG)GetTickCount64()) : 25;
                if (fresh && got == 0 && (LONGLONG)until - (LONGLONG)GetTickCount64() <= 0) break;
                if (l.dev->Read(r, wait) < 5) break;
                if (r[0] != 0x03 || r[2] != 0x40) continue;
                l.prev = l.last;
                l.prevFrameAt = l.lastFrameAt;
                l.last.assign(r.begin(), r.begin() + 5);
                l.lastFrameAt = GetTickCount64();
                got++;
            }
            if (!l.last.empty()) {
                if (!bestListener || l.lastFrameAt > bestListener->lastFrameAt) {
                    bestListener = &l;
                }
            }
        }
    }

    for (auto it = g_xenta.begin(); it != g_xenta.end();)  // forget receivers that were unplugged
        it = present.count(it->first) ? std::next(it) : g_xenta.erase(it);

    DeviceStatus s;
    s.key = L"xenta:delux_mouse";
    s.kind = DeviceKind::Mouse;
    s.graceSec = 86400;  // keep showing last known percentage while sleeping

    ULONGLONG now = GetTickCount64();
    bool hasRecentFrame = bestListener && !bestListener->last.empty() && (now - bestListener->lastFrameAt <= 15000);

    const XentaModel* model = nullptr;
    if (bestListener && !bestListener->last.empty()) {
        BYTE code = bestListener->last[1];
        for (const auto& m : kXentaModels)
            if (m.code == code) model = &m;
    }
    if (model) g_lastXentaName = model->name;
    s.name = !g_lastXentaName.empty() ? g_lastXentaName : DefaultXentaNameByPid(activePid);

    if (cablePresent) {
        s.via = L"Cabo USB";
        s.charging = true;
        if (hasRecentFrame) {
            BYTE st = bestListener->last[3];
            BYTE level = bestListener->last[4];
            int scaled = (model && model->tens) ? level * 10 : level;
            if (st == 0x02) {
                s.percent = 100;
                g_lastXentaPct = 100;
            } else if (scaled >= 1 && scaled <= 100) {
                s.percent = scaled;
                g_lastXentaPct = scaled;
            }
        }
        if (s.percent < 0 && g_lastXentaPct >= 0) s.percent = g_lastXentaPct;
    } else {
        s.via = (receiverPid == 0xFA65) ? L"Receptor 2.4 GHz (8K)" : L"Receptor 2.4 GHz";
        if (hasRecentFrame) {
            BYTE st = bestListener->last[3];
            BYTE level = bestListener->last[4];
            int scaled = (model && model->tens) ? level * 10 : level;
            if (st == 0x02) {
                s.charging = true;
                s.percent = 100;
                g_lastXentaPct = 100;
            } else if (st == 0x03) {
                s.charging = true;
                if (scaled >= 1 && scaled <= 100) {
                    s.percent = scaled;
                    g_lastXentaPct = scaled;
                } else if (g_lastXentaPct >= 0) {
                    s.percent = g_lastXentaPct;
                }
            } else if (st == 0x01 && scaled >= 1 && scaled <= 100) {
                s.charging = false;
                // Only filter a sudden 100% on the very first frame after wake/reconnect if previous wasn't 100%.
                bool wakeSpike100 = (scaled == 100 && g_lastXentaPct >= 0 && g_lastXentaPct < 98 &&
                                     (bestListener->prev.empty() || bestListener->prev[4] != level));
                if (!wakeSpike100) {
                    s.percent = scaled;
                    g_lastXentaPct = scaled;
                } else {
                    s.percent = g_lastXentaPct;
                }
            }
        } else {
            // Mouse is sleeping on the desk (receiver does not wake it up; we just keep the last reading visible).
            s.asleep = true;
            s.charging = false;
            s.percent = -1;
        }
    }

    for (USHORT pid : pids) {
        if (!ctx.IsClaimed(0x1D57, pid)) {
            auto allForPid = ctx.Find(0x1D57, pid, [](const HidColl&) { return true; });
            if (!allForPid.empty()) ctx.Claim(0x1D57, pid);
        }
    }
    ctx.out.push_back(s);
}

void PollAjazz(PollContext& ctx) {
    for (USHORT vendor : { (USHORT)0x249A, (USHORT)0x248A }) {
        for (auto [vid, pid] : ctx.Products(vendor)) {
            for (const HidColl* c : ctx.Find(vid, pid, [](const HidColl& c) { return c.iface == 2 && c.outLen() == 33 && c.inLen() == 33; })) {
                HidDev d;
                if (!d.Open(*c)) continue;
                d.Flush();
                if (!d.Write({ 0x00, 0x10 })) continue;  // report id 0, command 10, zero payload (checksum 0)
                std::vector<BYTE> r;
                bool ok = ReadUntil(d, r, 1500, [](const std::vector<BYTE>& r) {
                    if (r.size() < 32 || r[0] != 0x10 || r[1] != 0x00) return false;
                    unsigned sum = 0;
                    for (int i = 4; i <= 30; i++) sum += r[i];
                    return (BYTE)sum == r[31];
                });
                if (!ok || r[13] > 100) continue;
                DeviceStatus s;
                s.key = L"ajazz:" + Hex4(vid) + L":" + Hex4(pid);
                std::string model;
                for (int i = 4; i < 8 && r[i] >= 0x20 && r[i] < 0x7F; i++) model += (char)r[i];
                s.name = model.size() == 4 && model[0] == 'M' ? L"Ajazz AJ" + std::wstring(model.begin() + 1, model.end()) : L"Ajazz mouse";
                s.kind = DeviceKind::Mouse;
                bool receiver = vid == 0x249A;
                s.via = receiver ? L"2.4 GHz receiver" : L"USB cable";
                s.connected = !receiver || r[14] != 0;
                s.charging = r[12] != 0;
                if (s.connected) s.percent = r[13];
                ctx.Add(s, vid, pid);
                break;
            }
        }
    }
}

void PollAjazzAjSeries(PollContext& ctx) {
    for (auto [vid, pid] : ctx.Products(0x3151)) {
        for (const HidColl* c : ctx.Find(vid, pid, [](const HidColl& c) {
                 return c.page() == 0xFFFF && c.usage() == 0x02 && c.featLen() >= 65;
             })) {
            HidDev d;
            if (!d.OpenFeatureOnly(*c)) continue;
            std::vector<BYTE> req(c->featLen(), 0);
            req[0] = 0x00;  // unnumbered report
            req[1] = 0xF7;  // status poll: brings the 2.4 GHz telemetry link up
            if (!d.SetFeature(req)) continue;
            Sleep(30);
            std::vector<BYTE> r;
            if (!d.GetFeature(0x05, r) || r.size() < 4) continue;
            if (r[0] != 0x05 || r[1] != 0 || r[2] != 0) continue;  // malformed / reconnect frame
            int pct = r[3];
            if (pct > 100) continue;
            DeviceStatus s;
            s.key = L"ajazzaj:" + Hex4(vid) + L":" + Hex4(pid);
            s.name = HidProduct(*c);
            if (s.name.empty()) s.name = L"AJAZZ mouse";
            s.kind = DeviceKind::Mouse;
            s.via = L"2.4 GHz receiver";
            s.connected = pct > 0;  // 0 means the link is up but the mouse is off
            if (s.connected) s.percent = pct;
            ctx.Add(s, vid, pid);
            break;
        }
    }
}

}  // namespace

void PollMiscMice(PollContext& ctx) {
    PollMotospeed(ctx);
    PollXenta(ctx);
    PollAjazz(ctx);
    PollAjazzAjSeries(ctx);
}
