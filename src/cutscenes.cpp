#include "cutscenes.h"

#include <windows.h>

#include <atomic>
#include <cstdio>
#include <deque>
#include <mutex>

#include "features.h"
#include "game.h"
#include "log.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "util.h"

namespace cutscenes {
namespace {

// NTPlayerController.ENTCutsceneStatus
constexpr int kNone = 0;  // NTCutsceneStatus_NonePlaying (1 PlayingNoLoading, 2 PlayingAndLoading)

struct Seen {  // one cutscene, from the first frame the game reports it to the last
  std::string map;
  uint64_t startMs = 0, endMs = 0;
  int statusMax = 0;
  int tries = 0, accepted = 0;
  int firstAcceptMs = -1;  // after the start (-1: the game never accepted a skip)
  int frames = 0;
};

std::mutex g_mu;
std::deque<Seen> g_last;  // newest last, at most kKeep
constexpr size_t kKeep = 16;
int g_total = 0, g_skipped = 0, g_refused = 0;

Seen g_cur;
bool g_active = false;
bool g_probed = false;
bool g_lastAccepted = false;
std::atomic<bool> g_on{false};       // the feature is applied (OnEvent runs on every ProcessEvent)
std::atomic<bool> g_verbose{false};  // tests: what is playing, at the start of every cutscene
uintptr_t g_fnStartSkip = 0;    // NTSeqAct_Interp.StartSkip
float g_bypassAlpha = -1.0f;  // the screen's fade-in darkness while our TryCancel runs with the fade flag cleared
constexpr float kSkipFade = 0.05f;  // seconds; the game's own skip fades for FadeTime (1 s in M07)

bool Paused(const game::World& w) { return w.world && game::GetObj(w.world, "Pauser") != 0; }

int CutsceneStatus(uintptr_t pc) {
  engine::Call c(pc, "GetCutsceneStatus");
  return c.Invoke() ? c.RetByte() : kNone;
}

// The game's skip of a matinee (NTSeqAct_Interp, native) waits while the screen is still fading in from black
// (PlayerCamera.bEnableFading with FadeAmount > 0): after a load or a camera fade of the level script that is 1-3 s
// of the cutscene on screen. For our call only, the camera says "not fading"; the flag is put back right after, and
// StartSkip (OnEvent) then fades to black from where the fade-in was, so the screen does not flash.
bool TryCancel(uintptr_t pc, bool* bypassed) {
  uintptr_t cam = game::GetObj(pc, "PlayerCamera");
  bool fading = cam && game::GetBool(cam, "bEnableFading") && game::GetFloat(cam, "FadeAmount") > 0.0f;
  if (fading) {
    g_bypassAlpha = game::GetFloat(cam, "FadeAmount");
    game::SetBool(cam, "bEnableFading", false);
  }
  engine::Call c(pc, "TryCancelCutscene");  // a skip it starts runs StartSkip right inside this call (OnEvent)
  bool ok = c.Invoke() && c.RetBool();
  if (fading) {
    game::SetBool(cam, "bEnableFading", true);
    g_bypassAlpha = -1.0f;
  }
  *bypassed = fading;
  return ok;
}

void Finish(uint64_t now) {
  g_cur.endMs = now;
  LOG("cutscenes: cutscene over after %llu ms (%s, %d frames, %d tries, %d accepted, first accepted at %d ms)",
      (unsigned long long)(now - g_cur.startMs), g_cur.map.c_str(), g_cur.frames, g_cur.tries, g_cur.accepted,
      g_cur.firstAcceptMs);
  std::lock_guard<std::mutex> lk(g_mu);
  ++g_total;
  if (g_cur.accepted) ++g_skipped; else ++g_refused;
  g_last.push_back(g_cur);
  while (g_last.size() > kKeep) g_last.pop_front();
  g_active = false;
}

bool IsSub(uintptr_t o, uintptr_t cls) {
  for (uintptr_t c = ue3::ObjClass(o); c; c = ue3::StructSuper(c))
    if (c == cls) return true;
  return false;
}

class SkipCutscenes : public features::Feature {
 public:
  SkipCutscenes() {
    id = "skip_cutscenes";
    name = "Skip cutscenes";
    group = features::Group::Cutscenes;
    anywhere = true;
    help = "Skips cutscenes the moment they start: story scenes, boss intros, videos. The game ends them the same "
           "way as with its own Skip button (Backspace / Back), so doors, objectives and fights start as usual.\n"
           "Not skipped: what the game never lets you skip - a few short camera fly-overs (about 5 s) and the video "
           "in Mission 16. Loading screens stay.";
  }
  bool Enable(const features::Ctx&) override {
    if (!g_fnStartSkip) g_fnStartSkip = ue3::FindObject("Function", "StartSkip", "NTSeqAct_Interp");
    g_active = false;
    g_on = true;
    return true;
  }
  void Disable() override {
    g_on = false;
    if (g_active) Finish(GetTickCount64());
  }
  void Tick(const features::Ctx& c) override {
    uintptr_t pc = c.w.pc;
    if (!pc || !ue3::IsA(pc, "NTPlayerController")) return;
    uint64_t now = GetTickCount64();
    int st = CutsceneStatus(pc);
    if (st == kNone) {
      if (g_active) Finish(now);
      return;
    }
    if (!g_active) {
      g_cur = Seen{};
      g_cur.map = c.w.map;
      g_cur.startMs = now;
      g_active = true;
      g_probed = false;
      g_lastAccepted = false;
      LOG("cutscenes: cutscene started (%s, status %d, cinematic mode %d)", c.w.map.c_str(), st,
          game::GetBool(pc, "bCinematicMode"));
      if (g_verbose)  // a walk over every object: only for tests (autorun "cutscenes verbose")
        for (const std::string& l : Probe()) LOG("cutscenes:   %s", l.c_str());
    }
    if (st > g_cur.statusMax) g_cur.statusMax = st;
    ++g_cur.frames;
    if (Paused(c.w)) return;  // the game does not skip from its pause menu either
    ++g_cur.tries;
    // Called every frame while the cutscene lasts, like holding the button: the game starts its skip once (one
    // StartSkip per matinee) and keeps answering true while it ends it; a matinee chained after it gets its own.
    bool bypassed = false;
    bool ok = TryCancel(pc, &bypassed);
    if (ok) {
      ++g_cur.accepted;
      int at = (int)(now - g_cur.startMs);
      if (g_cur.firstAcceptMs < 0) g_cur.firstAcceptMs = at;
      if (!g_lastAccepted)
        LOG("cutscenes: skip accepted at %d ms (try %d, status %d%s)", at, g_cur.tries, st,
            bypassed ? ", screen was fading in" : "");
    } else if (g_lastAccepted) {
      LOG("cutscenes: skip refused again at %d ms (status %d)", (int)(now - g_cur.startMs), st);
    }
    g_lastAccepted = ok;
    if (!ok && !g_probed && now - g_cur.startMs > 2000 && g_cur.accepted == 0) {
      g_probed = true;
      LOG("cutscenes: the game refuses the skip for 2 s (status %d) - playing now:", st);
      for (const std::string& l : Probe()) LOG("cutscenes:   %s", l.c_str());
    }
  }
};

}  // namespace

features::Feature* CreateFeature() { return new SkipCutscenes; }

void OnEvent(uintptr_t self, uintptr_t fn) {
  if (!g_on || !fn || fn != g_fnStartSkip || !self) return;
  // StartSkip: ClientSetCameraFade(true, FadeColour, FadeDownAlpha, FadeTime, ...), EndSkip once FadeTimer ran out.
  float fade = game::GetFloat(self, "FadeTime", -1.0f);
  if (fade > kSkipFade) game::SetFloat(self, "FadeTime", kSkipFade);
  // FadeDownAlpha (Vector2D from -> to): start from the darkness the screen has now (fading in after a load).
  float from = -1.0f, now = -1.0f;
  game::Field fa = game::FieldOf(self, "FadeDownAlpha");
  uintptr_t pc = game::W().pc;
  uintptr_t cam = pc ? game::GetObj(pc, "PlayerCamera") : 0;
  if (fa.ok() && cam) {
    from = util::ReadOr<float>(self + fa.off);
    if (g_bypassAlpha >= 0.0f) now = g_bypassAlpha;
    else if (game::GetBool(cam, "bEnableFading")) now = game::GetFloat(cam, "FadeAmount");
    if (now > from) *reinterpret_cast<float*>(self + fa.off) = now;
  }
  LOG("cutscenes: %s.StartSkip - fade %.2f s -> %.2f s, from alpha %.2f (screen %.2f)", ue3::ObjName(self).c_str(),
      fade, fade > kSkipFade ? kSkipFade : fade, from, now);
}

void SetVerbose(bool on) { g_verbose = on; }

std::vector<std::string> DebugLines() {
  std::lock_guard<std::mutex> lk(g_mu);
  std::vector<std::string> out;
  char b[256];
  snprintf(b, sizeof b, "total %d skipped %d refused %d active %d", g_total, g_skipped, g_refused, (int)g_active);
  out.push_back(b);
  for (const Seen& s : g_last) {
    snprintf(b, sizeof b, "%s: %llu ms, status max %d, frames %d, tries %d, accepted %d, first at %d ms",
             s.map.c_str(), (unsigned long long)(s.endMs - s.startMs), s.statusMax, s.frames, s.tries, s.accepted,
             s.firstAcceptMs);
    out.push_back(b);
  }
  return out;
}

std::vector<std::string> Probe() {
  std::vector<std::string> out;
  static uintptr_t interp = 0, bink = 0;
  if (!interp) interp = ue3::FindObject("Class", "SeqAct_Interp");
  if (!bink) bink = ue3::FindObject("Class", "SeqAct_NT_StreamLevelsBehindBink");
  char b[320];
  for (int i = 0, n = ue3::NumObjects(); i < n; ++i) {
    uintptr_t o = ue3::Object(i);
    if (!o) continue;
    if (interp && IsSub(o, interp)) {
      if (!game::GetBool(o, "bIsPlaying")) continue;
      uintptr_t data = game::GetObj(o, "InterpData");
      snprintf(b, sizeof b, "matinee %s (%s) skippable %d pos %.2f / %.2f rate %.2f skipped %d level %s",
               ue3::ObjName(o).c_str(), ue3::ClassNameOf(o).c_str(), game::GetBool(o, "bIsSkippable"),
               game::GetFloat(o, "Position"), data ? game::GetFloat(data, "InterpLength") : -1.0f,
               game::GetFloat(o, "PlayRate"), game::GetBool(o, "bSkipped"), game::TopPackage(o).c_str());
      out.push_back(b);
    } else if (bink && IsSub(o, bink)) {
      if (!game::GetBool(o, "bBinkHasStartedPlaying") || game::GetBool(o, "bMovieFinished")) continue;
      snprintf(b, sizeof b, "bink %s '%s' allow skip %d blocking %d do skip %d level %s", ue3::ObjName(o).c_str(),
               game::GetStr(o, "BinkName").c_str(), game::GetBool(o, "m_AllowSkip"),
               game::GetBool(o, "bUseBlockingMovie"), game::GetBool(o, "bDoSkip"), game::TopPackage(o).c_str());
      out.push_back(b);
    }
  }
  if (out.empty()) out.push_back("no playing matinee / Bink action found");
  return out;
}

}  // namespace cutscenes
