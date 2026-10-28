// The features themselves. Research behind every choice: research/player_features.md.
#include <windows.h>
#include <Xinput.h>

#include <atomic>
#include <cmath>
#include <map>

#include "damagelog.h"
#include "enemies.h"
#include "features.h"
#include "input.h"
#include "log.h"
#include "proxy.h"
#include "state.h"
#include "ue3/engine.h"
#include "ue3/ue3.h"
#include "util.h"
#include "version.h"
#include "visuals.h"

namespace features {
namespace {

using game::Alive;
using game::GetBool;
using game::GetByte;
using game::GetFloat;
using game::GetObj;
using game::SetBool;
using game::SetFloat;

bool CallSimple(uintptr_t obj, const char* fn) {
  engine::Call c(obj, fn);
  return c.Invoke();
}

// ------------------------------------------------------------------ damage (one place for all hits)
// Every hit in the game goes through the event NTCombat_Nucleus.ContactInternal(Contact) of the victim's nucleus,
// called by native code through ProcessEvent; inside it DoDamage scales Contact.DamageAmount, clamps it to the
// minimum health and calls TakeDamage. We edit DamageAmount before the original runs.
struct DamageMods {
  std::atomic<bool> any{false};
  float dealt = 1, taken = 1;  // game thread
  bool dealtOn = false, takenOn = false, oneHitKills = false;
  uintptr_t fnDV = 0, fnNT = 0;
  void Update() { any = dealtOn || takenOn || oneHitKills; }
  void Resolve() {
    if (!fnDV) fnDV = ue3::FindObject("Function", "ContactInternal", "DVCombat_Nucleus");
    if (!fnNT) fnNT = ue3::FindObject("Function", "ContactInternal", "NTCombat_Nucleus");
  }
} g_dmg;

std::atomic<bool> g_hitLog{false};  // Dev/autorun: one log line per hit (groundwork for the damage log)

void OnContact(uintptr_t nucleus, uintptr_t contact) {
  uintptr_t player = game::W().pawn;
  if (!ue3::IsObject(contact)) return;
  uintptr_t victim = GetObj(nucleus, "Owner");
  uintptr_t attacker = GetObj(contact, "OriginatorActor");
  float dmg = GetFloat(contact, "DamageAmount");
  float out = dmg;
  if (victim == player) {
    if (g_dmg.takenOn) out *= g_dmg.taken;
  } else if (attacker == player && victim) {
    if (g_dmg.dealtOn) out *= g_dmg.dealt;
    // Larger than any health pool; DoDamage still applies armour, invulnerability and scripted minimum health,
    // so story-protected enemies (boss phases) behave as the game intends.
    if (g_dmg.oneHitKills && dmg > 0) out = std::max(out, 1.0e7f);
  }
  if (out != dmg) SetFloat(contact, "DamageAmount", out);
  damagelog::OnHit(game::W(), contact, attacker, victim, dmg, out);
  if (g_hitLog) {
    uintptr_t type = GetObj(contact, "DamageType");
    LOG("hit: %s -> %s  damage %g -> %g  type %s  victim HP %g", attacker ? ue3::ObjName(attacker).c_str() : "?",
        victim ? ue3::ObjName(victim).c_str() : "?", dmg, out, type ? ue3::ObjName(type).c_str() : "-",
        victim ? GetFloat(victim, "m_Health") : -1.0f);
    LOG("hit:   def '%s' style '%s'/'%s' group %d reaction '%s' orig %g minion %s", game::GetStr(contact, "DefinitionPathName").c_str(),
        game::GetName(contact, "StyleAttackName").c_str(), game::GetName(contact, "StyleDisplayName").c_str(),
        game::GetInt(contact, "ContactGroupID"), game::GetName(contact, "ReactionType").c_str(),
        GetFloat(contact, "OriginalDamageAmount"), ue3::ObjName(GetObj(contact, "OriginatorMinion")).c_str());
  }
}

// ------------------------------------------------------------------ Player: health & DT
class InfiniteHp : public Feature {
  uintptr_t ctrl_ = 0;
  bool orig_ = false;

 public:
  InfiniteHp() {
    id = "inf_hp";
    name = "Infinite HP";
    help = "Dante never loses health and cannot die from hits; his health bar stays full. Hits still knock him "
           "around. Wins over \"One-hit KO on player\".";
  }
  bool Enable(const Ctx& c) override {
    // The game's own god mode (Controller.bGodMode): NTCombat_Behaviour.TakeDamage and NTPawn.DealHealthDamage
    // skip the health change while it is set.
    ctrl_ = GetObj(c.w.pawn, "m_Controller");
    if (!ctrl_) return false;
    orig_ = GetBool(ctrl_, "bGodMode");
    SetBool(ctrl_, "bGodMode", true);
    return true;
  }
  void Tick(const Ctx& c) override {
    if (Alive(ctrl_) && !GetBool(ctrl_, "bGodMode")) SetBool(ctrl_, "bGodMode", true);
    float mx = GetFloat(c.w.pawn, "m_MaxHealth");
    if (mx > 0 && GetFloat(c.w.pawn, "m_Health") < mx) {
      engine::Call s(c.w.pawn, "SetHealth");  // the parameter is "Health" in NTPawn, "newHealth" in DVPawn
      s.Float(s.ParamName(0).c_str(), mx).Invoke();
    }
  }
  void Disable() override {
    if (Alive(ctrl_)) SetBool(ctrl_, "bGodMode", orig_);
    ctrl_ = 0;
  }
};

class OneHitKoPlayer : public Feature {
  uintptr_t cb_ = 0, pawn_ = 0;
  bool orig_ = false;
  // No Gold Orb revive: DVPawn_Player.AbleToResurrect() is false while m_ResurrectionDisabledByKismet is set (the
  // flag level scripts use through ForceDisableResurrection), so ShouldDie() lets Dante die and no orb is used.
  bool noRevive_ = false;   // we hold the flag
  bool gameFlag_ = false;   // the game's own value, put back when we let go

  void SetNoRevive(bool on) {
    if (on == noRevive_ || !Alive(pawn_)) return;
    if (on) {
      gameFlag_ = GetBool(pawn_, "m_ResurrectionDisabledByKismet");
      SetBool(pawn_, "m_ResurrectionDisabledByKismet", true);
    } else {
      SetBool(pawn_, "m_ResurrectionDisabledByKismet", gameFlag_);
    }
    noRevive_ = on;
    LOG("features: ohko_player: gold orb revive %s", on ? "blocked" : "back to the game");
  }

 public:
  OneHitKoPlayer() {
    id = "ohko_player";
    name = "One-hit KO on player";
    help = "Any hit that does damage kills Dante, like the Hell and Hell difficulty. Does nothing while "
           "\"Infinite HP\" is on.";
    loserTo = "inf_hp";
    params = {{"no_revive", "No Gold Orb revive", 0, 0, 1, "", ParamKind::Toggle, {},
               "When Dante dies, no Gold Orb is used and he does not come back: straight to the death screen. "
               "Off: the game uses a Gold Orb if you have one."}};
  }
  bool Enable(const Ctx& c) override {
    cb_ = GetObj(c.w.pawn, "CombatBehaviour");  // NTCombat_Behaviour.m_DieInOneHit, the game's Hell and Hell flag
    if (!cb_) return false;
    pawn_ = c.w.pawn;
    noRevive_ = false;
    orig_ = GetBool(cb_, "m_DieInOneHit");
    SetBool(cb_, "m_DieInOneHit", true);
    return true;
  }
  void Tick(const Ctx& c) override {
    if (Alive(cb_) && !GetBool(cb_, "m_DieInOneHit")) SetBool(cb_, "m_DieInOneHit", true);
    bool want = c.p[0] >= 0.5f && game::FieldOf(pawn_, "m_ResurrectionDisabledByKismet").ok();
    if (noRevive_ && want && !GetBool(pawn_, "m_ResurrectionDisabledByKismet")) {
      gameFlag_ = false;  // a level script allowed revives again: that is the game's value now
      SetBool(pawn_, "m_ResurrectionDisabledByKismet", true);
    }
    SetNoRevive(want);
  }
  void Disable() override {
    SetNoRevive(false);
    if (Alive(cb_)) SetBool(cb_, "m_DieInOneHit", orig_);
    cb_ = pawn_ = 0;
  }
};

class InfiniteDt : public Feature {
  uintptr_t tc_ = 0;
  bool orig_ = false;

 public:
  InfiniteDt() {
    id = "inf_dt";
    name = "Infinite DT";
    help = "The Devil Trigger gauge stays full and does not drain while Devil Trigger is active. Devil Trigger "
           "must already be unlocked in your game.";
  }
  bool Enable(const Ctx& c) override {
    // DVTriggerComponent.m_bCheatInfinitePoints: the game's own cheat flag, UseTriggerPoints skips the cost.
    tc_ = GetObj(c.w.pawn, "m_TriggerComponent");
    if (!tc_) return false;
    orig_ = GetBool(tc_, "m_bCheatInfinitePoints");
    SetBool(tc_, "m_bCheatInfinitePoints", true);
    return true;
  }
  void Tick(const Ctx&) override {
    if (!Alive(tc_)) return;
    if (!GetBool(tc_, "m_bCheatInfinitePoints")) SetBool(tc_, "m_bCheatInfinitePoints", true);
    // With the cheat flag the game never spends points but also stops filling the gauge (GiveMaxTriggerPoints
    // skips it): keep it full ourselves so the HUD shows a full bar.
    float mx = GetFloat(tc_, "m_MaxTriggerPoints");
    if (mx > 0 && GetFloat(tc_, "m_CurrentTriggerPoints") < mx) {
      SetFloat(tc_, "m_CurrentTriggerPoints", mx);
      CallSimple(tc_, "HUDSetBarFillValue");
    }
  }
  void Disable() override {
    if (Alive(tc_)) SetBool(tc_, "m_bCheatInfinitePoints", orig_);
    tc_ = 0;
  }
};

// ------------------------------------------------------------------ Player: movement
std::atomic<bool> g_glideTrace{false};  // Dev/autorun: log the glide physics every frame

class InfiniteGlide : public Feature {
  // DVPawn.DVJumpPhysics (EDVJumpPhysics): DVPHYS_AngelGlideBoost (3) for the boost dash, then DVPHYS_AngelGlide (4)
  // while the button is held. The game leaves it at 4 after landing, so "gliding" also needs NTPawn.NTPhysics =
  // NTPHYS_GravitationalJump (6, in the air; NTPHYS_Walking = 1 on the floor). Actor.Physics stays 1 in the air.
  static constexpr uint8_t kPhysGlide = 4, kNTPhysAir = 6;
  // DVPawn.Phys_AngelGlide (struct PhysicsAngelGlide, latest_2025 layout).
  static constexpr int kDesiredVel = 0x00, kSwoopAccel = 0x0C, kGlideTime = 0x20, kCurrentVel = 0x24;
  bool gliding_ = false;
  float z0_ = 0;

 public:
  InfiniteGlide() {
    id = "inf_glide";
    name = "Infinite Glide";
    help = "Angel Glide / Angel Boost can be used again and again in the air, without landing first.";
    params = {{"mode", "Mode", 0, 0, 1, "", ParamKind::Choice, {"Unlimited boosts", "Level flight"},
               "Unlimited boosts: as many glides and boosts as you like, Dante still sinks slowly while gliding.\n"
               "Level flight: the same, and Dante keeps his height while gliding. Let go, attack or land to "
               "leave the glide as usual."}};
  }
  bool Enable(const Ctx&) override {
    gliding_ = false;
    return true;
  }
  void Tick(const Ctx& c) override {
    uintptr_t pawn = c.w.pawn;
    // DVPawn.m_AngelGlideCount / m_AllowGlideBoost: what landing resets (DVPawn.ResetGlide / ResetGlideBoost).
    if (game::GetInt(pawn, "m_AngelGlideCount") != 0) game::SetInt(pawn, "m_AngelGlideCount", 0);
    if (!GetBool(pawn, "m_AllowGlideBoost")) SetBool(pawn, "m_AllowGlideBoost", true);

    bool level = c.p[0] >= 0.5f;
    uint8_t physics = GetByte(pawn, "NTPhysics");
    bool gliding = GetByte(pawn, "DVJumpPhysics") == kPhysGlide && physics == kNTPhysAir;
    game::Field loc = game::FieldOf(pawn, "Location"), vel = game::FieldOf(pawn, "Velocity"),
                pg = game::FieldOf(pawn, "Phys_AngelGlide");
    if (!loc.ok() || !vel.ok() || !pg.ok()) return;
    auto* L = reinterpret_cast<game::Vec*>(pawn + loc.off);
    auto* V = reinterpret_cast<game::Vec*>(pawn + vel.off);
    auto* G = reinterpret_cast<uint8_t*>(pawn + pg.off);
    if (gliding && !gliding_) z0_ = L->z;  // the height the glide started at
    gliding_ = gliding;
    if (g_glideTrace && (gliding || std::fabs(V->z) > 1)) {
      auto* d = reinterpret_cast<game::Vec*>(G + kDesiredVel);
      auto* cv = reinterpret_cast<game::Vec*>(G + kCurrentVel);
      auto* sw = reinterpret_cast<game::Vec*>(G + kSwoopAccel);
      LOG("glide: phys %u/%u z %.1f (z0 %.1f) vel %.0f,%.0f,%.0f desired %.0f,%.0f,%.0f current %.0f,%.0f,%.0f swoop "
          "%.0f,%.0f,%.0f t %.2f", GetByte(pawn, "DVJumpPhysics"), physics, L->z, z0_, V->x, V->y, V->z, d->x, d->y, d->z,
          cv->x, cv->y, cv->z, sw->x, sw->y, sw->z, *reinterpret_cast<float*>(G + kGlideTime));
    }
    if (!level || !gliding) return;
    // Level flight: the glide physics steers Dante towards vPhys_DesiredGlideVelocity, which the game points ~60
    // degrees down (500, 0, -866 at the start). With no vertical part in the target, the current glide velocity
    // and the swoop acceleration, the game's own physics flies level; horizontal speed and steering stay the
    // game's. Leaving the glide (button released, attack, landing) changes the physics mode and we stop.
    reinterpret_cast<game::Vec*>(G + kDesiredVel)->z = 0;
    reinterpret_cast<game::Vec*>(G + kCurrentVel)->z = 0;
    reinterpret_cast<game::Vec*>(G + kSwoopAccel)->z = 0;
    if (V->z < 0) V->z = 0;
  }
  void Disable() override { gliding_ = false; }
};

bool KeyHeld(int vk, int dik) {
  if (dinject::KeyHeld(dik)) return true;  // autorun tests
  if (state::menuOpen) return false;
  HWND fg = GetForegroundWindow();
  DWORD pid = 0;
  GetWindowThreadProcessId(fg, &pid);
  return pid == GetCurrentProcessId() && (GetAsyncKeyState(vk) & 0x8000) != 0;
}

class Noclip : public Feature {
  uintptr_t pawn_ = 0;
  uint8_t origPhys_ = 1;
  game::Vec pos_;
  std::vector<std::string> tokens_;  // tokens we created (owner 'Exec')

  static bool SetNTPhysics(uintptr_t pawn, uint8_t v) {
    engine::Call c(pawn, "SetNTPhysics");
    return c.Byte("newPhysics", v).Invoke();
  }
  static float CameraYaw(const game::World& w) {
    // Camera.CameraCache = { float TimeStamp; TPOV POV { Vector Location; Rotator Rotation; float FOV } }
    if (uintptr_t cam = GetObj(w.pc, "PlayerCamera")) {
      game::Field f = game::FieldOf(cam, "CameraCache");
      if (f.ok()) return util::ReadOr<int32_t>(cam + f.off + 4 + 12 + 4) * (6.2831853f / 65536.0f);
    }
    game::Field r = game::FieldOf(w.pc, "Rotation");
    return r.ok() ? util::ReadOr<int32_t>(w.pc + r.off + 4) * (6.2831853f / 65536.0f) : 0.0f;
  }

 public:
  Noclip() {
    id = "noclip";
    name = "Noclip";
    help = "Fly through walls. WASD to move (relative to the camera), Space up, Ctrl down, Shift faster. "
           "Gamepad: left stick, RT up, LT down. When you turn it off Dante drops from where he is.";
    params = {{"speed", "Fly speed", 1.0f, 0.25f, 5.0f, "%.2fx"}};
  }
  bool Enable(const Ctx& c) override {
    pawn_ = c.w.pawn;
    game::Field loc = game::FieldOf(pawn_, "Location");
    if (!loc.ok()) return false;
    pos_ = util::ReadOr<game::Vec>(pawn_ + loc.off);
    origPhys_ = GetByte(pawn_, "NTPhysics");
    SetNTPhysics(pawn_, 0);  // NTPHYS_None: no gravity, no walking/collision physics
    tokens_.clear();
    // The game's own movement/jump/evade denial: WASD and Space do not also drive Dante's legs.
    for (const char* t : {"MovementDenial", "JumpDenial", "EvadeDenial"})
      if (ue3::FindNameIndex(t) >= 0 && !game::HasToken(pawn_, t, "Exec") && game::CreateToken(pawn_, t, "Exec"))
        tokens_.push_back(t);
    return true;
  }
  void Tick(const Ctx& c) override {
    if (!Alive(pawn_)) return;
    if (GetByte(pawn_, "NTPhysics") != 0) SetNTPhysics(pawn_, 0);
    game::Field loc = game::FieldOf(pawn_, "Location");
    if (loc.ok()) {  // moved by someone else (Teleport / Load position, a level script): fly on from there
      game::Vec now = util::ReadOr<game::Vec>(pawn_ + loc.off);
      if (std::fabs(now.x - pos_.x) + std::fabs(now.y - pos_.y) + std::fabs(now.z - pos_.z) > 100) pos_ = now;
    }
    float dt = c.w.gameDt > 0 ? c.w.realDt : 0.0f;  // frozen while the game is paused
    float fwd = 0, right = 0, up = 0;
    if (KeyHeld('W', 0x11)) fwd += 1;
    if (KeyHeld('S', 0x1F)) fwd -= 1;
    if (KeyHeld('D', 0x20)) right += 1;
    if (KeyHeld('A', 0x1E)) right -= 1;
    if (KeyHeld(VK_SPACE, 0x39)) up += 1;
    if (KeyHeld(VK_CONTROL, 0x1D)) up -= 1;
    bool fast = KeyHeld(VK_SHIFT, 0x2A);
    XINPUT_STATE xs{};
    if (!state::menuOpen && input::RawPadState(&xs)) {
      auto axis = [](SHORT v) { return std::abs(v) < 7849 ? 0.0f : v / 32767.0f; };
      fwd += axis(xs.Gamepad.sThumbLY);
      right += axis(xs.Gamepad.sThumbLX);
      up += xs.Gamepad.bRightTrigger / 255.0f - xs.Gamepad.bLeftTrigger / 255.0f;
    }
    float speed = 1200.0f * c.p[0] * (fast ? 3.0f : 1.0f) * dt;
    float yaw = CameraYaw(c.w);
    float cy = std::cos(yaw), sy = std::sin(yaw);
    pos_.x += (cy * fwd - sy * right) * speed;
    pos_.y += (sy * fwd + cy * right) * speed;
    pos_.z += up * speed;
    engine::Call m(pawn_, "ForcedSetLocation");
    m.Raw("NewLocation", &pos_, sizeof pos_).Bool("UpdateComponents", true).Invoke();
    game::Field vel = game::FieldOf(pawn_, "Velocity");
    if (vel.ok()) *reinterpret_cast<game::Vec*>(pawn_ + vel.off) = {};
  }
  void Disable() override {
    if (Alive(pawn_)) {
      SetNTPhysics(pawn_, origPhys_ ? origPhys_ : 1);  // walking physics: Dante falls and lands normally
      for (const std::string& t : tokens_) game::DeleteToken(pawn_, t.c_str(), "Exec");
    }
    tokens_.clear();
    pawn_ = 0;
  }
};

// Multiplies a time dilation the game also writes (hit stops, Devil Trigger, scripted slow motion): whenever the
// value differs from what we wrote last, the game changed it and that becomes the new base.
struct ScaledFloat {
  float base = 1, last = -1;
  void Apply(uintptr_t obj, const char* field, float mult) {
    float cur = GetFloat(obj, field, 1.0f);
    if (last < 0 || std::fabs(cur - last) > 1e-5f) base = cur;
    last = base * mult;
    SetFloat(obj, field, last);
  }
  void Restore(uintptr_t obj, const char* field) {
    if (Alive(obj) && last >= 0 && std::fabs(GetFloat(obj, field, 1.0f) - last) < 1e-5f) SetFloat(obj, field, base);
    last = -1;
  }
};

class PlayerSpeed : public Feature {
  uintptr_t pawn_ = 0;
  ScaledFloat v_;

 public:
  PlayerSpeed() {
    id = "player_speed";
    name = "Player speed";
    help = "Speeds up or slows down only Dante (moves, attacks, animations). Stacks with Game speed.";
    params = {{"mult", "Speed", 1.0f, 0.1f, 3.0f, "%.1fx"}};
  }
  bool Enable(const Ctx& c) override {
    pawn_ = c.w.pawn;
    v_ = {};
    return true;
  }
  void Tick(const Ctx& c) override {
    if (Alive(pawn_)) v_.Apply(pawn_, "CustomTimeDilation", c.p[0]);
  }
  void Disable() override {
    v_.Restore(pawn_, "CustomTimeDilation");
    pawn_ = 0;
  }
};

class DamageTaken : public Feature {
 public:
  DamageTaken() {
    id = "dmg_taken";
    name = "Damage taken";
    help = "Multiplies the damage Dante takes from every hit. 0x = no damage, 2x = double.";
    params = {{"mult", "Multiplier", 1.0f, 0.0f, 10.0f, "%.1fx"}};
  }
  bool Enable(const Ctx&) override {
    g_dmg.Resolve();
    g_dmg.takenOn = true;
    g_dmg.Update();
    return true;
  }
  void Tick(const Ctx& c) override { g_dmg.taken = c.p[0]; }
  void Disable() override {
    g_dmg.takenOn = false;
    g_dmg.Update();
  }
};

// ------------------------------------------------------------------ Enemies
class OneHitKills : public Feature {
 public:
  OneHitKills() {
    id = "ohk_enemies";
    name = "One-hit kills";
    group = Group::Enemies;
    help = "Any damaging hit from Dante kills the enemy. Shields, armour and story-protected bosses still work "
           "as in the game. Does nothing while \"Infinite enemy HP\" is on.";
    loserTo = "enemy_inf_hp";
  }
  bool Enable(const Ctx&) override {
    g_dmg.Resolve();
    g_dmg.oneHitKills = true;
    g_dmg.Update();
    return true;
  }
  void Disable() override {
    g_dmg.oneHitKills = false;
    g_dmg.Update();
  }
};

class DamageDealt : public Feature {
 public:
  DamageDealt() {
    id = "dmg_dealt";
    name = "Damage dealt";
    group = Group::Enemies;
    help = "Multiplies the damage Dante does to enemies. 0x = no damage, 2x = double.";
    params = {{"mult", "Multiplier", 1.0f, 0.0f, 10.0f, "%.1fx"}};
  }
  bool Enable(const Ctx&) override {
    g_dmg.Resolve();
    g_dmg.dealtOn = true;
    g_dmg.Update();
    return true;
  }
  void Tick(const Ctx& c) override { g_dmg.dealt = c.p[0]; }
  void Disable() override {
    g_dmg.dealtOn = false;
    g_dmg.Update();
  }
};

std::vector<uintptr_t> ActiveEnemies(const game::World& w) {
  std::vector<uintptr_t> out;
  uintptr_t em = GetObj(w.gameInfo, "m_EnemyManager");  // DVGameInfo.m_EnemyManager.m_ActiveEnemies
  if (!em) return out;
  game::Field f = game::FieldOf(em, "m_ActiveEnemies");
  ue3::TArrayRaw a{};
  if (!f.ok() || !util::Read(em + f.off, a) || a.num <= 0 || a.num > 512 || !a.data) return out;
  for (int i = 0; i < a.num; ++i) {
    uintptr_t p = util::ReadOr<uintptr_t>(a.data + i * 4);
    if (p && p != w.pawn && ue3::IsObject(p)) out.push_back(p);
  }
  return out;
}

class EnemySpeed : public Feature {
  std::map<uintptr_t, ScaledFloat> v_;

 public:
  EnemySpeed() {
    id = "enemy_speed";
    name = "Enemy speed";
    group = Group::Enemies;
    help = "Speeds up or slows down only the enemies. Stacks with Game speed.";
    params = {{"mult", "Speed", 1.0f, 0.1f, 3.0f, "%.1fx"}};
  }
  bool Enable(const Ctx&) override {
    v_.clear();
    return true;
  }
  void Tick(const Ctx& c) override {
    for (uintptr_t e : ActiveEnemies(c.w)) v_[e].Apply(e, "CustomTimeDilation", c.p[0]);
    for (auto it = v_.begin(); it != v_.end();)
      it = Alive(it->first) ? std::next(it) : v_.erase(it);
  }
  void Disable() override {
    for (auto& [e, v] : v_) v.Restore(e, "CustomTimeDilation");
    v_.clear();
  }
};

// ------------------------------------------------------------------ Game
class GameSpeed : public Feature {
  uintptr_t world_ = 0;
  ScaledFloat v_;

 public:
  GameSpeed() {
    id = "game_speed";
    name = "Game speed";
    group = Group::Game;
    help = "Speeds up or slows down the whole game (Dante, enemies, effects). Player speed and Enemy speed are "
           "applied on top of it.";
    params = {{"mult", "Speed", 1.0f, 0.1f, 10.0f, "%.1fx"}};
  }
  bool Enable(const Ctx& c) override {
    world_ = c.w.world;  // WorldInfo.TimeDilation, what the game's own "Slomo" command sets
    v_ = {};
    return world_ != 0;
  }
  void Tick(const Ctx& c) override {
    if (Alive(world_)) v_.Apply(world_, "TimeDilation", c.p[0]);
  }
  void Disable() override {
    v_.Restore(world_, "TimeDilation");
    world_ = 0;
  }
};

}  // namespace

std::vector<Feature*> CreateNavFeatures();    // nav_features.cpp
std::vector<Feature*> CreateEnemyFeatures();  // enemy_features.cpp
std::vector<Feature*> CreateVisualFeatures();  // visual_features.cpp

std::vector<Feature*> CreateAll() {
  // Menu order inside each group: most used first.
  std::vector<Feature*> all = {new InfiniteHp,     new InfiniteDt,  new InfiniteGlide, new Noclip,
                               new OneHitKoPlayer, new DamageTaken, new PlayerSpeed,   new OneHitKills,
                               new DamageDealt,    new EnemySpeed,  new GameSpeed};
  for (Feature* f : CreateNavFeatures()) all.push_back(f);
  for (Feature* f : CreateEnemyFeatures()) all.push_back(f);
  for (Feature* f : CreateVisualFeatures()) all.push_back(f);
  return all;
}

void SetHitLog(bool on) { g_hitLog = on; }
void SetGlideTrace(bool on) { g_glideTrace = on; }

void OnGameEvent(void* self, void* fn, void* parms) {
  if (!FeaturesAllowed()) return;
  if (visuals::WantsEvents()) visuals::OnEvent((uintptr_t)self, (uintptr_t)fn, parms, false);
  if ((!g_dmg.any && !g_hitLog && !damagelog::On()) || !parms) return;
  if (!g_dmg.fnDV) g_dmg.Resolve();
  uintptr_t f = (uintptr_t)fn;
  if (!f || (f != g_dmg.fnDV && f != g_dmg.fnNT)) return;
  OnContact((uintptr_t)self, *reinterpret_cast<uintptr_t*>(parms));
}

// After the game processed a hit: the victim's health and the damage after armour (damage log).
void OnGameEventAfter(void* self, void* fn, void* parms) {
  if (!FeaturesAllowed()) return;
  enemies::OnEventAfter((uintptr_t)self, (uintptr_t)fn);
  if (visuals::WantsEvents()) visuals::OnEvent((uintptr_t)self, (uintptr_t)fn, parms, true);
  if (!damagelog::On() || !parms) return;
  uintptr_t f = (uintptr_t)fn;
  if (!f || (f != g_dmg.fnDV && f != g_dmg.fnNT)) return;
  damagelog::AfterHit((uintptr_t)self, *reinterpret_cast<uintptr_t*>(parms));
}

}  // namespace features
