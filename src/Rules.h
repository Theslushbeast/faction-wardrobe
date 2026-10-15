#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <string_view>
#include <algorithm>
#include <numeric>
namespace Wardrobe {
struct Outfit { std::string name="New outfit"; int chance=0,sex=0,slot=-1; std::vector<std::string> pieces; }; // sex: Both=0 Male=1 Female=2
struct Rule { std::string faction; int priority=0; bool enabled=true; std::vector<std::string> male,female; int slotMale=-1,slotFemale=-1; bool pool=false; std::vector<Outfit> outfits; };
struct Config { bool enabled=true, replaceAll=false; std::vector<Rule> rules; };
inline bool EligibleOutfit(const Outfit& o,bool female) {return o.sex==0 || o.sex==(female?2:1);}
inline int Total(const Rule& r,bool female) {int n=0;for(auto& o:r.outfits)if(EligibleOutfit(o,female))n+=o.chance;return n;}
inline std::string PoolError(const Rule& r) {
    if(!r.pool)return {};
    for(auto& o:r.outfits) {
        if(o.sex<0 || o.sex>2 || o.chance<0 || o.chance>100)return "Faction share must be 0-100 and eligibility must be Male, Female or Both.";
        if(o.chance>0 && (o.name.empty() || o.pieces.empty()))return "Every positive-chance outfit needs a name and armor pieces.";
    }
    for(bool female:{false,true}) {
        int total=Total(r,female);bool eligible=false;
        for(auto& o:r.outfits)if(EligibleOutfit(o,female))eligible=true;
        if(eligible && total!=100)return female?"Female outfit shares must total 100%.":"Male outfit shares must total 100%.";
    }
    return {};
}
inline std::uint64_t StableHash(std::string_view s) {
    std::uint64_t h=14695981039346656037ull;for(unsigned char c:s){h^=c;h*=1099511628211ull;}
    h^=h>>30;h*=0xbf58476d1ce4e5b9ull;h^=h>>27;h*=0x94d049bb133111ebull;return h^(h>>31);
}
inline const Outfit* Pick(const Rule& r,bool female,std::uint64_t roll) {
    if(!r.pool || !PoolError(r).empty() || Total(r,female)!=100)return nullptr;
    int point=static_cast<int>(roll%100),end=0;
    for(auto& o:r.outfits)if(EligibleOutfit(o,female)){end+=o.chance;if(point<end)return &o;}
    return nullptr;
}
// Largest remainder apportionment: exact total headcount, deterministic ties in outfit order.
inline std::vector<std::size_t> Quotas(const std::vector<int>& weights,std::size_t members) {
    std::vector<std::size_t> result(weights.size(),0),order(weights.size());
    if(weights.empty() || std::any_of(weights.begin(),weights.end(),[](int w){return w<0 || w>100;}) || std::accumulate(weights.begin(),weights.end(),0)!=100)return result;
    std::iota(order.begin(),order.end(),0);std::size_t used=0;
    for(std::size_t i=0;i<weights.size();++i){result[i]=members*weights[i]/100;used+=result[i];}
    std::stable_sort(order.begin(),order.end(),[&](auto a,auto b){return members*weights[a]%100>members*weights[b]%100;});
    for(std::size_t i=0;i<members-used;++i)++result[order[i]];
    return result;
}
inline std::vector<int> EligibleIndices(const Rule& rule,bool female) {
    std::vector<int> result;
    if(!PoolError(rule).empty() || Total(rule,female)!=100)return result;
    for(int i=0;i<static_cast<int>(rule.outfits.size());++i)if(rule.outfits[i].chance>0 && EligibleOutfit(rule.outfits[i],female))result.push_back(i);
    return result;
}
inline std::vector<std::pair<std::string,int>> Allocate(const Rule& rule,std::vector<std::string> members,const std::vector<int>& indices) {
    std::vector<int> weights;for(int i:indices){if(i<0 || i>=static_cast<int>(rule.outfits.size()))return {};weights.push_back(rule.outfits[i].chance);}
    if(std::accumulate(weights.begin(),weights.end(),0)!=100)return {};
    std::sort(members.begin(),members.end());members.erase(std::unique(members.begin(),members.end()),members.end());
    std::sort(members.begin(),members.end(),[&](const auto& a,const auto& b){auto ha=StableHash(rule.faction+"|"+a),hb=StableHash(rule.faction+"|"+b);return ha==hb?a<b:ha<hb;});
    auto targets=Quotas(weights,members.size());std::vector<std::pair<std::string,int>> result;std::size_t pos=0;
    for(std::size_t i=0;i<indices.size();++i)for(std::size_t n=0;n<targets[i];++n)result.emplace_back(members[pos++],indices[i]);
    return result;
}
inline std::vector<std::pair<int*,std::vector<std::string>*>> Bindings(Rule& r) {
    std::vector<std::pair<int*,std::vector<std::string>*>> out;
    if(r.pool){for(auto& o:r.outfits)out.emplace_back(&o.slot,&o.pieces);}
    else {out.emplace_back(&r.slotMale,&r.male);out.emplace_back(&r.slotFemale,&r.female);}
    return out;
}
template<class Member> const Rule* Winner(const Config& config,bool female,Member member) {
    if(!config.enabled)return nullptr;
    const Rule* winner=nullptr;
    for(auto& rule:config.rules) {
        const auto& pieces=female?rule.female:rule.male;
        bool available=rule.pool?(PoolError(rule).empty() && Total(rule,female)==100):!pieces.empty();
        if(!rule.enabled || !available || !member(rule.faction))continue;
        if(!winner || rule.priority>winner->priority)winner=&rule;
    }
    return winner;
}
}
