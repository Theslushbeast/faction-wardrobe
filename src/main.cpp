// Faction Wardrobe - SKSE plugin
// Copyright (C) 2026 Clyde
// SPDX-License-Identifier: GPL-3.0-or-later
//
// Gives every member of a faction an outfit you pick in-game (SKSE Menu Framework). The outfits are real outfit
// records from FactionWardrobe.esp, filled at runtime, so the game keeps NPCs dressed by itself.
#include <RE/Skyrim.h>
#include <SKSE/SKSE.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <mutex>
#include <map>
#include <set>
#include <atomic>
#include <thread>
#include <chrono>
#include "SKSEMenuFramework.h"
#include "Rules.h"

namespace Wardrobe {
using Json=nlohmann::json;
struct Entry { std::string key,label,plugin; std::uint32_t slots=0; };
struct State { std::vector<std::string> original,added,applied; std::string signature; };
std::recursive_mutex mutex;
Config config;
Config appliedConfig;
std::vector<Entry> factions,armors;
std::vector<std::string> plugins;
std::map<RE::FormID,State> states; // Main game thread and SKSE serialization only.
std::atomic<bool> ready=false;
std::atomic<std::uint64_t> epoch=0;
std::atomic<bool> sweepPending=false;
std::jthread watcher;
std::string status="Loading data...";
const std::filesystem::path path="Data/SKSE/Plugins/FactionWardrobe.json";

std::string Key(RE::TESForm* f) {
    if(!f || !f->GetFile(0)) return {};
    return std::format("{}|{:06X}",f->GetFile(0)->fileName,f->GetLocalFormID());
}
template<class T> T* Resolve(const std::string& key) {
    auto pos=key.rfind('|'); if(pos==std::string::npos) return nullptr;
    try { auto id=std::stoul(key.substr(pos+1),nullptr,16); return RE::TESDataHandler::GetSingleton()->LookupForm<T>(static_cast<RE::FormID>(id),key.substr(0,pos)); }
    catch(...) { return nullptr; }
}
std::string Label(RE::TESForm* f) {
    auto name=f->GetName();auto editor=f->GetFormEditorID();
    std::string text=name && *name?name:editor && *editor?editor:"Unnamed record";
    return text+" ["+Key(f)+"]";
}
Json Encode(const Config& c) {
    Json j={{"version",3},{"distribution","factionQuotas"},{"enabled",c.enabled},{"replaceAllArmor",c.replaceAll},{"rules",Json::array()}};
    for(auto& r:c.rules) {
        Json v={{"faction",r.faction},{"priority",r.priority},{"enabled",r.enabled},{"male",r.male},{"female",r.female},{"slotMale",r.slotMale},{"slotFemale",r.slotFemale},{"pool",r.pool},{"outfits",Json::array()}};
        for(auto& o:r.outfits)v["outfits"].push_back({{"name",o.name},{"chance",o.chance},{"sex",o.sex},{"slot",o.slot},{"pieces",o.pieces}});
        j["rules"].push_back(std::move(v));
    }
    return j;
}
void LoadConfig() {
    if(!std::filesystem::exists(path)) return;
    try {
        std::ifstream stream(path);auto j=Json::parse(stream);Config next;
        next.enabled=j.value("enabled",true);next.replaceAll=j.value("replaceAllArmor",false);
        for(auto& v:j.at("rules")) {
            Rule r;r.faction=v.at("faction").get<std::string>();r.priority=v.value("priority",0);r.enabled=v.value("enabled",true);
            r.male=v.value("male",std::vector<std::string>{});r.slotMale=v.value("slotMale",-1);r.slotFemale=v.value("slotFemale",-1);r.female=v.value("female",std::vector<std::string>{});
            r.pool=v.value("pool",false);
            if(v.contains("outfits"))for(auto& x:v.at("outfits")){Outfit o;o.name=x.value("name",std::string("Outfit"));o.chance=x.value("chance",0);o.sex=x.value("sex",0);o.slot=x.value("slot",-1);o.pieces=x.value("pieces",std::vector<std::string>{});r.outfits.push_back(std::move(o));}
            if(auto error=PoolError(r);!error.empty())throw std::runtime_error(r.faction+": "+error);
            next.rules.push_back(std::move(r));
        }
        config=std::move(next);
    } catch(const std::exception& e) { config.enabled=false;status=std::string("Config error; distribution disabled: ")+e.what(); }
}
bool SaveConfig() { // UI mutex held; replace atomically, keep previous settings as .bak.
    try {
        std::filesystem::create_directories(path.parent_path());
        auto temp=path;temp+=".tmp";
        std::ofstream stream(temp,std::ios::binary|std::ios::trunc);stream<<Encode(config).dump(2);stream.flush();
        if(!stream) throw std::runtime_error("Cannot write settings");stream.close();
        if(std::filesystem::exists(path)) std::filesystem::copy_file(path,path.string()+".bak",std::filesystem::copy_options::overwrite_existing);
        if(!MoveFileExW(temp.c_str(),path.c_str(),MOVEFILE_REPLACE_EXISTING|MOVEFILE_WRITE_THROUGH)) throw std::runtime_error("Cannot replace settings file");
        return true;
    } catch(const std::exception& e) {status=std::string("Save failed: ")+e.what();return false;}
}
void Catalog() {
    std::scoped_lock lock(mutex);auto data=RE::TESDataHandler::GetSingleton();
    for(auto f:data->GetFormArray<RE::TESFaction>()) if(f && !Key(f).empty()) factions.push_back({Key(f),Label(f),f->GetFile(0)->fileName});
    std::set<std::string> files;
    for(auto a:data->GetFormArray<RE::TESObjectARMO>()) if(a && !Key(a).empty()) {
        std::string plugin=a->GetFile(0)->fileName;
        armors.push_back({Key(a),Label(a),plugin,static_cast<std::uint32_t>(a->GetSlotMask().underlying())});files.insert(plugin);
    }
    auto sort=[](const Entry& a,const Entry& b){return a.label<b.label;};
    std::sort(factions.begin(),factions.end(),sort);std::sort(armors.begin(),armors.end(),sort);
    plugins.assign(files.begin(),files.end());
    status=std::format("{} factions, {} armor records from {} loaded plugins.",factions.size(),armors.size(),plugins.size());
}
bool Eligible(RE::Actor* a) {
    return a && !a->IsPlayerRef() && !a->IsDead() && a->Is3DLoaded() && a->GetActorBase() && !a->IsChild();
}
void Restore(RE::Actor* actor,State& s) {
    auto manager=RE::ActorEquipManager::GetSingleton();
    for(auto& key:s.applied) if(auto armor=Resolve<RE::TESObjectARMO>(key)) {
        auto inv=actor->GetInventory();auto it=inv.find(armor);
        if(it!=inv.end() && it->second.second && it->second.second->IsWorn() && !it->second.second->IsQuestObject()) manager->UnequipObject(actor,armor,nullptr,1,nullptr,false,false,false,true);
    }
    // Remove only one copy of each item that this plugin had to add.
    for(auto& key:s.added) if(auto armor=Resolve<RE::TESObjectARMO>(key)) {
        auto inv=actor->GetInventory();auto it=inv.find(armor);
        if(it!=inv.end() && it->second.first>0 && it->second.second && !it->second.second->IsQuestObject()) {
            manager->UnequipObject(actor,armor,nullptr,1,nullptr,false,false,false,true);
            actor->RemoveItem(armor,1,RE::ITEM_REMOVE_REASON::kRemove,nullptr,nullptr);
        }
    }
    for(auto& key:s.original) if(auto armor=Resolve<RE::TESObjectARMO>(key)) {
        auto inv=actor->GetInventory();auto it=inv.find(armor);
        if(it!=inv.end() && it->second.first>0) manager->EquipObject(actor,armor,nullptr,1,nullptr,false,false,false,true);
    }
}
// ---------------------------------------------------------------- real outfits (0.2)
// FactionWardrobe.esp holds 128 empty outfit records. Each faction rule gets one per sex, filled with the chosen
// armor at runtime, and set as the default outfit of the faction members' NPC records. The engine then keeps them
// dressed by itself (resetinventory, cell resets, reloads). The outfit change is never written into the save:
// it is undone around every save and redone after it, so removing the mod leaves vanilla outfits behind.
constexpr auto kPlugin="FactionWardrobe.esp";
constexpr int kSlots=128;
constexpr RE::FormID kFirstSlot=0x800;
std::vector<RE::BGSOutfit*> slots;            // index -> outfit record from the esp
std::set<int> badSlots;                        // filled from a rule with a missing armor record: not used
std::map<RE::TESNPC*,RE::BGSOutfit*> originals; // NPC record -> its own outfit, for every record we changed
std::map<RE::TESNPC*,RE::FormID> owners;        // which actor decided a shared NPC record's outfit
std::set<RE::FormID> sharedWarned;

void LoadSlots() {
    auto dh=RE::TESDataHandler::GetSingleton();
    slots.assign(kSlots,nullptr);int found=0;
    for(int i=0;i<kSlots;++i) if((slots[i]=dh->LookupForm<RE::BGSOutfit>(kFirstSlot+i,kPlugin)))++found;
    if(!found) for(auto o:dh->GetFormArray<RE::BGSOutfit>()) // LookupForm can miss in some setups
        if(o && o->GetFile(0) && std::string_view(o->GetFile(0)->fileName)==kPlugin) {
            auto i=static_cast<int>(o->GetLocalFormID())-static_cast<int>(kFirstSlot);
            if(i>=0 && i<kSlots && !slots[i]) {slots[i]=o;++found;}
        }
    SKSE::log::info("{}: {} outfit slots",kPlugin,found);
    if(!found) status=std::string("FactionWardrobe.esp is not enabled: outfits can't be applied.");
}
bool IsSlot(RE::BGSOutfit* o) {return o && std::find(slots.begin(),slots.end(),o)!=slots.end();}
RE::BGSOutfit* Slot(int i) {return i>=0 && i<static_cast<int>(slots.size()) && !badSlots.contains(i)?slots[i]:nullptr;}

using GroupKey=std::pair<std::string,std::vector<int>>;
std::map<GroupKey,std::vector<std::string>> quotaGroups; // main thread only
std::map<std::pair<std::string,std::string>,int> quotaAssignments;
std::map<std::string,std::string> quotaSummaries; // UI mutex
std::string Identity(RE::TESNPC* npc) {
    auto key=Key(npc);return key.empty()?std::format("dynamic:{:08X}",npc->GetFormID()):key;
}
bool BaseMember(RE::TESNPC* npc,RE::TESFaction* faction) {
    for(int depth=0;npc && depth<16;++depth) {
        if(npc->actorData.templateUseFlags.all(RE::ACTOR_BASE_DATA::TEMPLATE_USE_FLAG::kFactions) && npc->baseTemplateForm) {
            if(auto parent=npc->baseTemplateForm->As<RE::TESNPC>()){npc=parent;continue;}
        }
        return npc->IsInFaction(faction);
    }
    return false;
}
void SummarizeQuotas(const Config& cfg) {
    std::scoped_lock lock(mutex);quotaSummaries.clear();
    for(auto& rule:cfg.rules)if(rule.pool) {
        std::vector<int> counts(rule.outfits.size(),0);int total=0;
        for(auto& [key,index]:quotaAssignments)if(key.first==rule.faction && index>=0 && index<static_cast<int>(counts.size())){++counts[index];++total;}
        std::string text=std::format("Applied roster: {} known NPC bases (includes unloaded records).",total);
        for(std::size_t i=0;i<counts.size();++i)text+=std::format("  {}: {}",rule.outfits[i].name,counts[i]);
        quotaSummaries[rule.faction]=std::move(text);
    }
}
void BuildQuotas(const Config& cfg) {
    quotaGroups.clear();quotaAssignments.clear();
    std::map<RE::TESNPC*,RE::Actor*> live;
    if(auto processes=RE::ProcessLists::GetSingleton())processes->ForAllActors([&](RE::Actor* a){
        if(a && !a->IsPlayerRef() && a->GetActorBase()) {auto& current=live[a->GetActorBase()];if(!current || a->GetFormID()<current->GetFormID())current=a;}
        return RE::BSContainer::ForEachResult::kContinue;
    });
    auto player=RE::PlayerCharacter::GetSingleton();
    auto add=[&](RE::TESNPC* npc) {
        if(!npc || npc->IsPreset() || (player && npc==player->GetActorBase()))return;
        auto race=npc->GetRace();if(!race || race->IsChildRace())return;
        RE::Actor* actor=live.contains(npc)?live[npc]:(npc->IsUnique()?npc->GetUniqueActor():nullptr);
        if(actor && actor->IsDead())return;
        bool female=npc->GetSex()==RE::SEX::kFemale;
        auto rule=Winner(cfg,female,[&](const std::string& key){auto f=Resolve<RE::TESFaction>(key);return f && (actor?actor->IsInFaction(f):BaseMember(npc,f));});
        if(rule && rule->pool)quotaGroups[{rule->faction,EligibleIndices(*rule,female)}].push_back(Identity(npc));
    };
    for(auto npc:RE::TESDataHandler::GetSingleton()->GetFormArray<RE::TESNPC>())add(npc);
    for(auto& [npc,actor]:live)add(npc); // includes runtime bases absent from the plugin form arrays
    for(auto& [group,members]:quotaGroups) {
        std::sort(members.begin(),members.end());members.erase(std::unique(members.begin(),members.end()),members.end());
        auto rule=std::find_if(cfg.rules.begin(),cfg.rules.end(),[&](auto& r){return r.faction==group.first;});
        if(rule!=cfg.rules.end())for(auto& [member,index]:Allocate(*rule,members,group.second))quotaAssignments[{group.first,member}]=index;
    }
    SummarizeQuotas(cfg);
}
int QuotaChoice(const Rule& rule,RE::TESNPC* npc,bool female,const Config& cfg) {
    auto identity=Identity(npc);auto indices=EligibleIndices(rule,female);
    auto key=std::pair{rule.faction,identity};
    if(auto it=quotaAssignments.find(key);it!=quotaAssignments.end() && std::find(indices.begin(),indices.end(),it->second)!=indices.end())return rule.outfits[it->second].slot;
    if(indices.empty())return -1;
    // Newly discovered/runtime membership: fill the largest deficit without reshuffling existing members.
    GroupKey group{rule.faction,indices};
    for(auto& [other,members]:quotaGroups)if(other.first==rule.faction)std::erase(members,identity);
    auto& members=quotaGroups[group];std::vector<int> weights;std::vector<std::size_t> counts(indices.size(),0);
    for(auto i:indices)weights.push_back(rule.outfits[i].chance);
    for(auto& member:members)if(auto it=quotaAssignments.find({rule.faction,member});it!=quotaAssignments.end())for(std::size_t j=0;j<indices.size();++j)if(indices[j]==it->second)++counts[j];
    auto target=Quotas(weights,members.size()+1);std::size_t best=0;
    for(std::size_t j=1;j<indices.size();++j)if(static_cast<std::int64_t>(target[j])-static_cast<std::int64_t>(counts[j])>static_cast<std::int64_t>(target[best])-static_cast<std::int64_t>(counts[best]))best=j;
    quotaAssignments[key]=indices[best];members.push_back(identity);SummarizeQuotas(cfg);
    return rule.outfits[indices[best]].slot;
}

// Gives every rule/sex with pieces its own slot, keeping the slots it already has. UI mutex held.
bool AssignSlots(Config& c) {
    for(auto& r:c.rules)if(auto error=PoolError(r);!error.empty()){status=r.faction+": "+error;return false;}
    std::set<int> used;
    for(auto& r:c.rules) for(auto [s,pieces]:Bindings(r)) if(*s>=0) {if(*s>=kSlots || used.contains(*s))*s=-1;else used.insert(*s);}
    bool ok=true;
    for(auto& r:c.rules) for(auto [s,pieces]:Bindings(r)) {
        if(pieces->empty() || *s>=0) continue;
        int i=0;while(i<kSlots && used.contains(i))++i;
        if(i==kSlots){ok=false;continue;}
        *s=i;used.insert(i);
    }
    if(!ok)status="All 128 outfit slots are in use: remove a faction rule first.";
    return ok;
}
// Puts the selected armor into the slot records. Main thread.
void Fill(const Config& c) {
    badSlots.clear();std::set<int> filled;
    auto draft=c;
    for(auto& r:draft.rules) for(auto [slotIndex,pieces]:Bindings(r)) {
        auto s=*slotIndex;
        if(s<0 || s>=static_cast<int>(slots.size()) || !slots[s] || pieces->empty()) continue;
        std::vector<RE::TESObjectARMO*> armor;
        for(auto& key:*pieces) if(auto a=Resolve<RE::TESObjectARMO>(key))armor.push_back(a);
        auto& items=slots[s]->outfitItems;items.clear();
        if(armor.size()!=pieces->size()) {badSlots.insert(s);SKSE::log::warn("{}: an armor record is missing, outfit skipped",r.faction);continue;}
        for(auto a:armor)items.push_back(a);
        filled.insert(s);
    }
    for(int i=0;i<static_cast<int>(slots.size());++i) if(slots[i] && !filled.contains(i))slots[i]->outfitItems.clear();
}
// Removes every item the engine handed out for this outfit (also pieces since taken out of it).
void Purge(RE::Actor* actor,RE::BGSOutfit* outfit) {
    auto changes=actor->GetInventoryChanges();
    if(!outfit || !changes || !changes->entryList) return;
    std::vector<std::pair<RE::TESBoundObject*,RE::ExtraDataList*>> hits;
    for(auto entry:*changes->entryList) {
        if(!entry || !entry->object || !entry->extraLists) continue;
        for(auto x:*entry->extraLists) if(x) if(auto tag=x->GetByType<RE::ExtraOutfitItem>();tag && tag->id==outfit->GetFormID()) hits.emplace_back(entry->object,x);
    }
    for(auto [object,x]:hits) actor->RemoveItem(object,(std::max)(1,x->GetCount()),RE::ITEM_REMOVE_REASON::kRemove,x,nullptr);
}
// When an NPC loads: drops armor handed out for an outfit it no longer has (a reverted or changed faction),
// then dresses it in its current outfit again if anything was taken off.
void CleanStale(RE::Actor* actor) {
    if(!Eligible(actor)) return;
    auto npc=actor->GetActorBase();auto changes=actor->GetInventoryChanges();
    if(!changes || !changes->entryList) return;
    std::vector<std::pair<RE::TESBoundObject*,RE::ExtraDataList*>> hits;
    for(auto entry:*changes->entryList) {
        if(!entry || !entry->object || !entry->extraLists) continue;
        for(auto x:*entry->extraLists) if(x) if(auto tag=x->GetByType<RE::ExtraOutfitItem>()) {
            auto o=RE::TESForm::LookupByID<RE::BGSOutfit>(tag->id);
            if(!IsSlot(o)) continue;
            bool current=o==npc->defaultOutfit;
            if(current) {current=false;for(auto f:o->outfitItems)if(f==entry->object)current=true;}
            if(!current)hits.emplace_back(entry->object,x);
        }
    }
    if(hits.empty()) return;
    for(auto [object,x]:hits) actor->RemoveItem(object,(std::max)(1,x->GetCount()),RE::ITEM_REMOVE_REASON::kRemove,x,nullptr);
    if(npc->defaultOutfit && !actor->IsDisabled()) actor->AddWornOutfit(npc->defaultOutfit,true);
}
void Dress(RE::Actor* actor,RE::BGSOutfit* from,RE::BGSOutfit* to,bool replaceAll) {
    auto npc=actor->GetActorBase();
    if(from) {if(IsSlot(from))Purge(actor,from);else actor->RemoveOutfitItems(from);}
    npc->defaultOutfit=to;  // in memory only (no change flag): see SaveGuard
    actor->InitInventoryIfRequired();
    if(!to || actor->IsDisabled()) return;
    actor->AddWornOutfit(to,true);
    if(!replaceAll || !IsSlot(to)) return;
    auto manager=RE::ActorEquipManager::GetSingleton();
    for(auto& [object,data]:actor->GetInventory()) if(auto a=object->As<RE::TESObjectARMO>();a && data.second && data.second->IsWorn() && !data.second->IsQuestObject()) {
        bool mine=false;for(auto f:to->outfitItems)if(f==a)mine=true;
        if(!mine)manager->UnequipObject(actor,a,nullptr,1,nullptr,false,false,false,true);
    }
}
// redress: the slot's armor changed (Save & apply), so dress again even if the record already points at it.
void Apply(RE::Actor* actor,const Config& cfg,bool redress=false) {
    if(!Eligible(actor)) return;
    auto id=actor->GetFormID();
    if(auto old=states.find(id);old!=states.end()) {Restore(actor,old->second);states.erase(old);} // 0.1 leftovers
    auto npc=actor->GetActorBase();
    bool female=npc->GetSex()==RE::SEX::kFemale;
    const Rule* rule=Winner(cfg,female,[actor](const std::string& key){auto f=Resolve<RE::TESFaction>(key);return f && actor->IsInFaction(f);});
    int chosen=-1;
    if(rule) {
        if(rule->pool) {
            chosen=QuotaChoice(*rule,npc,female,cfg);
        } else chosen=female?rule->slotFemale:rule->slotMale;
    }
    RE::BGSOutfit* wanted=Slot(chosen);
    // A configured but unavailable outfit is not a request to strip/revert this NPC.
    if(rule && !wanted)return;
    auto known=originals.find(npc);
    if(!wanted && known==originals.end()) return;          // never touched, nothing configured
    RE::BGSOutfit* target=wanted?wanted:known->second;
    // Several actors can share one NPC record; the first one loaded decides, so they don't flip it back and forth.
    if(auto o=owners.find(npc);o!=owners.end() && o->second!=id && npc->defaultOutfit!=target) {
        if(auto first=RE::TESForm::LookupByID<RE::Actor>(o->second);first && first->Is3DLoaded() && first->GetActorBase()==npc) {
            if(sharedWarned.insert(id).second)SKSE::log::info("{:08X} shares its NPC record with {:08X}; keeping that one's outfit",id,o->second);
            return;
        }
    }
    if(npc->defaultOutfit==target && !(redress && wanted)) return;
    if(known==originals.end())originals[npc]=npc->defaultOutfit;
    Dress(actor,npc->defaultOutfit,target,cfg.replaceAll);
    if(wanted)owners[npc]=id;else {originals.erase(npc);owners.erase(npc);}
}
// Puts every changed NPC record back to its own outfit (records only, nobody is redressed).
std::vector<std::pair<RE::TESNPC*,RE::BGSOutfit*>> UndoRecords() {
    std::vector<std::pair<RE::TESNPC*,RE::BGSOutfit*>> ours;
    for(auto& [npc,orig]:originals) {ours.emplace_back(npc,npc->defaultOutfit);npc->defaultOutfit=orig;}
    return ours;
}
void SaveGuard() { // kSaveGame: the save gets the vanilla outfit records; ours go back right after
    auto ours=UndoRecords();
    SKSE::GetTaskInterface()->AddTask([ours]{for(auto& [npc,o]:ours)if(originals.contains(npc))npc->defaultOutfit=o;});
}
void ResetSession() {UndoRecords();originals.clear();owners.clear();sharedWarned.clear();quotaGroups.clear();quotaAssignments.clear();}
Config Snapshot() { std::scoped_lock lock(mutex);return appliedConfig; }
void ApplyLoaded(bool redress,bool clean=false) {
    if(!ready) return;auto cfg=Snapshot();
    if(auto list=RE::ProcessLists::GetSingleton()) list->ForAllActors([&](RE::Actor* a){Apply(a,cfg,redress);if(clean)CleanStale(a);return RE::BSContainer::ForEachResult::kContinue;});
}
// After a revert: NPC records whose actor isn't loaded go back to their own outfit now. Their leftover armor is
// removed by CleanStale when they next load.
void ReleaseUnwanted(const Config& cfg) {
    for(auto it=originals.begin();it!=originals.end();) {
        auto actor=owners.contains(it->first)?RE::TESForm::LookupByID<RE::Actor>(owners[it->first]):nullptr;
        if(actor && actor->Is3DLoaded()) {++it;continue;}
        bool used=false;
        auto draft=cfg;
        for(auto& r:draft.rules)for(auto [sl,pieces]:Bindings(r))if(cfg.enabled && r.enabled && !pieces->empty() && Slot(*sl)==it->first->defaultOutfit)used=true;
        if(used){++it;continue;}
        it->first->defaultOutfit=it->second;owners.erase(it->first);it=originals.erase(it);
    }
}
void QueueRevert() {
    Config next;{std::scoped_lock lock(mutex);next=config;}
    auto token=epoch.load();SKSE::GetTaskInterface()->AddTask([token,next]{if(ready && epoch==token){Fill(next);BuildQuotas(next);{std::scoped_lock lock(mutex);appliedConfig=next;}ApplyLoaded(false,true);ReleaseUnwanted(next);}});
}
void QueueApply(bool refill,bool redress) {
    Config next;{std::scoped_lock lock(mutex);next=config;}
    auto token=epoch.load();SKSE::GetTaskInterface()->AddTask([token,refill,redress,next]{if(ready && epoch==token){if(refill){Fill(next);BuildQuotas(next);}{std::scoped_lock lock(mutex);appliedConfig=next;}ApplyLoaded(redress,refill);ReleaseUnwanted(next);}});
}
void StartWatcher() {
    watcher=std::jthread([](std::stop_token stop){
        while(!stop.stop_requested()) {
            for(int i=0;i<3 && !stop.stop_requested();++i)std::this_thread::sleep_for(std::chrono::seconds(1));
            if(stop.stop_requested() || !ready || sweepPending.exchange(true))continue;
            auto token=epoch.load();
            SKSE::GetTaskInterface()->AddTask([token]{
                sweepPending=false;
                if(ready && epoch==token) {auto ui=RE::UI::GetSingleton();if(!ui || !ui->GameIsPaused())ApplyLoaded(false);}
            });
        }
    });
}
struct Events:RE::BSTEventSink<RE::TESObjectLoadedEvent> {
    RE::BSEventNotifyControl ProcessEvent(const RE::TESObjectLoadedEvent* e,RE::BSTEventSource<RE::TESObjectLoadedEvent>*) override {
        if(e && e->loaded && ready) {auto id=e->formID;auto token=epoch.load();SKSE::GetTaskInterface()->AddTask([id,token]{if(ready && epoch==token) if(auto a=RE::TESForm::LookupByID<RE::Actor>(id)){Apply(a,Snapshot());CleanStale(a);}});}
        return RE::BSEventNotifyControl::kContinue;
    }
} events;
bool Match(const std::string& text,const char* filter) {
    std::string a=text,b=filter;
    auto lower=[](unsigned char c){return static_cast<char>(std::tolower(c));};
    std::transform(a.begin(),a.end(),a.begin(),lower);std::transform(b.begin(),b.end(),b.begin(),lower);
    return a.find(b)!=std::string::npos;
}
void __stdcall Render() {
    std::scoped_lock lock(mutex);
    static char factionSearch[128]{},armorSearch[128]{};static std::string faction,plugin;static int sex=0,outfitIndex=0;static bool confirmAll=false;std::string revert;
    ImGui::TextWrapped("Faction Wardrobe: faction-wide outfit proportions, not independent rolls. Each eligible member gets one complete set. Highest faction priority wins.");
    ImGui::TextWrapped("%s",status.c_str());
    if(!config.enabled) {
        ImGui::TextColored(ImVec4(1.0f,0.45f,0.35f,1.0f),"Faction Wardrobe is OFF: nobody gets these outfits until you turn it back on.");
        if(ImGui::Button("Turn back on & apply")) {config.enabled=true;if(AssignSlots(config) && SaveConfig()){QueueApply(true,true);status="On again. Applying to loaded NPCs.";}}
    }
    ImGui::Checkbox("Distribution enabled",&config.enabled);
    ImGui::Checkbox("Replace all worn armor (otherwise only conflicting slots)",&config.replaceAll);
    ImGui::InputText("Find faction",factionSearch,sizeof(factionSearch));
    auto display=std::find_if(factions.begin(),factions.end(),[&](auto& e){return e.key==faction;});
    if(ImGui::BeginCombo("Faction",display==factions.end()?"Choose faction":display->label.c_str())) {
        for(auto& f:factions) if(Match(f.label,factionSearch)) if(ImGui::Selectable(f.label.c_str(),f.key==faction)) faction=f.key;
        ImGui::EndCombo();
    }
    if(!faction.empty()) {
        auto it=std::find_if(config.rules.begin(),config.rules.end(),[&](auto& r){return r.faction==faction;});
        if(it==config.rules.end()) {if(ImGui::Button("Configure this faction")) {config.rules.push_back(Rule{faction});} }
        else {
            auto& rule=*it;ImGui::Checkbox("Faction rule enabled",&rule.enabled);ImGui::InputInt("Priority",&rule.priority);
            ImGui::Checkbox("Use faction-wide outfit pool",&rule.pool);
            std::vector<std::string>* selection=nullptr;
            if(rule.pool) {
                if(rule.outfits.empty()) {
                    if(!rule.male.empty())rule.outfits.push_back(Outfit{"Existing male outfit",100,1,rule.slotMale,rule.male});
                    if(!rule.female.empty())rule.outfits.push_back(Outfit{"Existing female outfit",100,2,rule.slotFemale,rule.female});
                    if(rule.outfits.empty())rule.outfits.push_back(Outfit{"Outfit 1",100,0,-1,{}});
                }
                outfitIndex=std::clamp(outfitIndex,0,static_cast<int>(rule.outfits.size())-1);
                if(ImGui::BeginCombo("Outfit to edit",rule.outfits[outfitIndex].name.c_str())) {
                    for(int i=0;i<static_cast<int>(rule.outfits.size());++i){ImGui::PushID(i);if(ImGui::Selectable(rule.outfits[i].name.c_str(),i==outfitIndex))outfitIndex=i;ImGui::PopID();}
                    ImGui::EndCombo();
                }
                if(ImGui::Button("Create outfit")){rule.outfits.push_back(Outfit{"Outfit "+std::to_string(rule.outfits.size()+1),0,0,-1,{}});outfitIndex=static_cast<int>(rule.outfits.size())-1;}
                ImGui::SameLine();if(ImGui::Button("Duplicate outfit")){auto copy=rule.outfits[outfitIndex];copy.name+=" copy";copy.slot=-1;copy.chance=0;rule.outfits.push_back(std::move(copy));outfitIndex=static_cast<int>(rule.outfits.size())-1;}
                ImGui::SameLine();if(ImGui::Button("Delete selected outfit")) {
                    if(rule.outfits.size()>1){rule.outfits.erase(rule.outfits.begin()+outfitIndex);outfitIndex=0;}
                    else status="Use Revert this faction to remove its last outfit.";
                }
                auto& outfit=rule.outfits[outfitIndex];char name[192]{};
                std::copy_n(outfit.name.data(),(std::min)(outfit.name.size(),sizeof(name)-1),name);
                if(ImGui::InputText("Outfit name",name,sizeof(name)))outfit.name=name;
                const char* sexes[]={"Both","Male","Female"};
                if(ImGui::BeginCombo("Eligible members",sexes[std::clamp(outfit.sex,0,2)])) {for(int i=0;i<3;++i)if(ImGui::Selectable(sexes[i],outfit.sex==i))outfit.sex=i;ImGui::EndCombo();}
                if(ImGui::InputInt("Faction share (%)",&outfit.chance))outfit.chance=std::clamp(outfit.chance,0,100);
                ImGui::TextWrapped("Pool totals: Male %d%% | Female %d%%. Both-eligible outfits count toward each total. An absent sex pool leaves that sex unchanged.",Total(rule,false),Total(rule,true));
                if(auto error=PoolError(rule);!error.empty())ImGui::TextColored(ImVec4(1,.45f,.35f,1),"%s",error.c_str());
                ImGui::TextWrapped("Shares are rounded to whole members across the known roster, including unloaded records. Both sexes are counted together when their eligible lists match; otherwise each group is balanced separately. Apply rebuilds quotas; merely entering a cell does not reshuffle existing assignments. Shared NPC bases count once.");
                if(auto q=quotaSummaries.find(rule.faction);q!=quotaSummaries.end())ImGui::TextWrapped("%s",q->second.c_str());
                selection=&outfit.pieces;
            } else {
                if(ImGui::BeginCombo("Sex",sex?"Female":"Male")) {if(ImGui::Selectable("Male",sex==0))sex=0;if(ImGui::Selectable("Female",sex==1))sex=1;ImGui::EndCombo();}
                selection=sex?&rule.female:&rule.male;
            }
            auto& selected=*selection;
            if(ImGui::BeginCombo("Armor source plugin",plugin.empty()?"All loaded armor plugins":plugin.c_str())) {
                if(ImGui::Selectable("All loaded armor plugins",plugin.empty()))plugin.clear();
                for(auto& p:plugins)if(ImGui::Selectable(p.c_str(),p==plugin))plugin=p;
                ImGui::EndCombo();
            }
            ImGui::InputText("Find armor",armorSearch,sizeof(armorSearch));
            ImGui::TextWrapped("Selected: %zu pieces. You may mix pieces from different plugins. Conflicting slots are rejected.",selected.size());
            ImGui::BeginChild("Selected pieces",ImVec2(0,130));
            for(std::size_t i=0;i<selected.size();) {
                auto e=std::find_if(armors.begin(),armors.end(),[&](auto& v){return v.key==selected[i];});
                ImGui::PushID(static_cast<int>(i));bool remove=ImGui::Button("Remove");ImGui::SameLine();
                ImGui::TextWrapped("%s",e==armors.end()?selected[i].c_str():e->label.c_str());ImGui::PopID();
                if(remove)selected.erase(selected.begin()+i);else ++i;
            }ImGui::EndChild();
            ImGui::BeginChild("Armor records",ImVec2(0,260));
            int shown=0;
            for(auto& a:armors) if((plugin.empty() || a.plugin==plugin) && Match(a.label,armorSearch)) {
                if(++shown>250){ImGui::TextWrapped("Refine the search to see more than 250 results.");break;}
                bool checked=std::find(selected.begin(),selected.end(),a.key)!=selected.end();
                if(ImGui::Checkbox(a.label.c_str(),&checked)) {
                    if(!checked) std::erase(selected,a.key);
                    else {
                        bool conflict=false;for(auto& k:selected) {auto other=std::find_if(armors.begin(),armors.end(),[&](auto& e){return e.key==k;});if(other!=armors.end() && (other->slots&a.slots))conflict=true;}
                        if(conflict)status="Slot conflict: remove the selected item using that slot first.";
                        else {selected.push_back(a.key);status="Selection updated; Save & apply when ready.";}
                    }
                }
            }ImGui::EndChild();
            if(!rule.pool && ImGui::Button("Copy this selection to other sex")) {if(sex)rule.male=rule.female;else rule.female=rule.male;}
            ImGui::SameLine();if(ImGui::Button("Revert this faction")) revert=rule.faction;
        }
    }
    if(!revert.empty()) { // removes the faction's rule; its members go back to their own outfits
        std::erase_if(config.rules,[&](auto& r){return r.faction==revert;});
        if(SaveConfig()) {QueueRevert();status="Faction reverted: its members are back in their own outfits (others as they load).";}
    }
    if(ImGui::Button("Save & apply") && (config.enabled=true) && AssignSlots(config) && SaveConfig()) {status="Saved. Applying to loaded NPCs; other NPCs update when loaded.";QueueApply(true,true);}
    ImGui::SameLine();if(ImGui::Button("Apply now") && (config.enabled=true) && AssignSlots(config)) {QueueApply(true,true);status="Applying current selections to loaded NPCs (not saved to disk).";}
    if(config.enabled && ImGui::Button("Turn off: everyone back in their own outfits")) {config.enabled=false;if(SaveConfig()){QueueApply(true,false);status="Off. Loaded NPCs are back in their own outfits; others change back as they load. Your outfit settings are kept.";}}
    if(!config.rules.empty()) {
        if(!confirmAll) {if(ImGui::Button("Revert all factions"))confirmAll=true;}
        else {
            ImGui::TextColored(ImVec4(1.0f,0.45f,0.35f,1.0f),"Delete every faction outfit and put everyone back in their own outfits?");
            if(ImGui::Button("Yes, revert all")) {config.rules.clear();confirmAll=false;if(SaveConfig()){QueueRevert();status="Everything reverted: NPCs are back in their own outfits (unloaded ones as they load). Previous settings kept in FactionWardrobe.json.bak.";}}
            ImGui::SameLine();if(ImGui::Button("Cancel"))confirmAll=false;
        }
    }
    ImGui::TextWrapped("Loaded ESP/ESM/ESL armor is indexed by originating plugin. Settings are global. Draft edits take effect only when applied. FactionWardrobe.esp supplies up to 128 active outfit slots. Disable/revert before uninstalling and keep a backup save.");
}
constexpr std::uint32_t record=0x46574431;
void SaveState(SKSE::SerializationInterface* s) {
    Json data=Json::array();for(auto& [id,v]:states)data.push_back({{"actor",id},{"original",v.original},{"added",v.added},{"applied",v.applied},{"signature",v.signature}});
    auto text=data.dump();if(s->OpenRecord(record,1))s->WriteRecordData(text.data(),static_cast<std::uint32_t>(text.size()));
}
void LoadState(SKSE::SerializationInterface* s) {
    states.clear();std::uint32_t type,version,length;
    while(s->GetNextRecordInfo(type,version,length)) {
        if(type!=record || version!=1 || length>16*1024*1024)continue;
        std::string text(length,'\0');if(s->ReadRecordData(text.data(),length)!=length)continue;
        try {for(auto& v:Json::parse(text)) {RE::FormID id;if(!s->ResolveFormID(v.at("actor").get<RE::FormID>(),id))continue;State st;st.original=v.at("original").get<std::vector<std::string>>();st.added=v.at("added").get<std::vector<std::string>>();st.applied=v.value("applied",std::vector<std::string>{});st.signature=v.at("signature").get<std::string>();states[id]=std::move(st);}} catch(...) {SKSE::log::error("Invalid FactionWardrobe co-save record");}
    }
}
}
SKSE_PLUGIN_LOAD(const SKSE::LoadInterface* skse) {
    if(skse->RuntimeVersion()!=REL::Version(1,6,1170,0))return false;
    SKSE::Init(skse);Wardrobe::LoadConfig();
    auto serial=SKSE::GetSerializationInterface();serial->SetUniqueID(0x46574442);serial->SetSaveCallback(Wardrobe::SaveState);serial->SetLoadCallback(Wardrobe::LoadState);
    serial->SetRevertCallback([](SKSE::SerializationInterface*){Wardrobe::ready=false;++Wardrobe::epoch;Wardrobe::states.clear();Wardrobe::ResetSession();});
    SKSE::GetMessagingInterface()->RegisterListener([](SKSE::MessagingInterface::Message* m){
        using namespace Wardrobe;
        if(m->type==SKSE::MessagingInterface::kDataLoaded) {
            Catalog();LoadSlots();{std::scoped_lock lock(mutex);if(!AssignSlots(config))config.enabled=false;appliedConfig=config;}RE::ScriptEventSourceHolder::GetSingleton()->AddEventSink(&events);StartWatcher();
            if(GetModuleHandleW(L"SKSEMenuFramework.dll") && SKSEMenuFramework::IsInstalled()) {SKSEMenuFramework::SetSection("Faction Wardrobe");SKSEMenuFramework::AddSectionItem("Outfits",Render);}
        }
        if(m->type==SKSE::MessagingInterface::kPreLoadGame) {ready=false;++epoch;}
        if(m->type==SKSE::MessagingInterface::kSaveGame) SaveGuard();
        if(m->type==SKSE::MessagingInterface::kPostLoadGame || m->type==SKSE::MessagingInterface::kNewGame) {ready=true;QueueApply(true,false);}
    });return true;
}
