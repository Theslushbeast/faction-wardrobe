#include "Rules.h"
#include <iostream>
#include <stdexcept>
using namespace Wardrobe;
int main() {
    int checks=0;
    auto require=[&](bool result){++checks;if(!result)throw std::runtime_error("Rule regression failed");};
    Config c;auto member=[](auto& name){return name!="unrelated";};
    require(Winner(c,false,member)==nullptr);
    c.rules={{"guards",1,true,{"maleArmor"},{"femaleArmor"}},{"soldiers",5,true,{"uniform"},{}}};
    require(Winner(c,false,member)==&c.rules[1]);
    require(Winner(c,true,member)==&c.rules[0]);
    c.rules[1].priority=1;require(Winner(c,false,member)==&c.rules[0]);
    c.rules[0].enabled=false;require(Winner(c,false,member)==&c.rules[1]);
    require(Winner(c,true,member)==nullptr);
    c.rules[1].faction="unrelated";require(Winner(c,false,member)==nullptr);
    c.rules[0].enabled=true;c.rules[0].priority=-999;require(Winner(c,false,member)==&c.rules[0]);
    c.enabled=false;require(Winner(c,false,member)==nullptr);
    Rule pool;pool.pool=true;pool.faction="companions";
    pool.outfits={{"Paladin",50,0,1,{"armorA"}},{"Barbarian",25,0,2,{"armorB"}},{"Viking",25,0,3,{"armorC"}}};
    require(PoolError(pool).empty());require(Total(pool,false)==100 && Total(pool,true)==100);
    int counts[3]{};
    for(unsigned i=0;i<100;++i){auto p=Pick(pool,false,i);require(p!=nullptr);++counts[p-&pool.outfits[0]];require(Pick(pool,true,i)==p);}
    require(counts[0]==50 && counts[1]==25 && counts[2]==25);
    require(Pick(pool,false,49)==&pool.outfits[0]);require(Pick(pool,false,50)==&pool.outfits[1]);
    require(Pick(pool,false,74)==&pool.outfits[1]);require(Pick(pool,false,75)==&pool.outfits[2]);
    pool.outfits[0].chance=49;require(!PoolError(pool).empty());require(Pick(pool,false,0)==nullptr);
    pool.outfits[0].chance=51;require(!PoolError(pool).empty());pool.outfits[0].chance=50;
    pool.outfits[0].pieces.clear();require(!PoolError(pool).empty());pool.outfits[0].pieces={"armorA"};
    for(auto& o:pool.outfits)o.sex=1;
    require(PoolError(pool).empty());require(Pick(pool,true,20)==nullptr);
    pool.outfits.push_back({"Female",100,2,4,{"armorD"}});require(PoolError(pool).empty());require(Pick(pool,true,20)==&pool.outfits[3]);
    pool.outfits.push_back({"Disabled",0,0,5,{}});require(PoolError(pool).empty());
    pool.outfits.back().chance=-1;require(!PoolError(pool).empty());pool.outfits.pop_back();
    c.enabled=true;c.rules={pool};require(Winner(c,true,member)==&c.rules[0]);
    require(StableHash("Skyrim.esm|001234|companions|M")==StableHash("Skyrim.esm|001234|companions|M"));
    auto first=Pick(pool,false,StableHash("Skyrim.esm|001234|companions|M"));
    for(int i=0;i<10;++i)require(Pick(pool,false,StableHash("Skyrim.esm|001234|companions|M"))==first);
    require(Bindings(pool).size()==4);
    Rule legacy;legacy.male={"oldArmor"};legacy.slotMale=7;auto b=Bindings(legacy);require(b.size()==2 && *b[0].first==7 && *b[0].second==legacy.male);
    require(Quotas({50,50},10)==std::vector<std::size_t>({5,5}));
    require(Quotas({50,25,25},7)==std::vector<std::size_t>({3,2,2}));
    require(Quotas({50,50},5)==std::vector<std::size_t>({3,2}));
    require(Quotas({50,50},1)==std::vector<std::size_t>({1,0}));
    require(Quotas({50,50},0)==std::vector<std::size_t>({0,0}));
    require(Quotas({0,100},17)==std::vector<std::size_t>({0,17}));
    require(Quotas({50,49},10)==std::vector<std::size_t>({0,0}));
    require(Quotas({-1,101},10)==std::vector<std::size_t>({0,0}));
    for(std::size_t n=0;n<500;++n) {
        auto quotas=Quotas({50,25,25},n);require(std::accumulate(quotas.begin(),quotas.end(),std::size_t{})==n);
        for(int i=0;i<3;++i){auto scaled=n*(i==0?50:25);require(quotas[i]==scaled/100 || quotas[i]==(scaled+99)/100);}
    }
    Rule balanced;balanced.pool=true;balanced.faction="companions";
    balanced.outfits={{"Paladin",50,0,1,{"armorA"}},{"Plague",50,0,2,{"armorB"}}};
    require(EligibleIndices(balanced,false)==EligibleIndices(balanced,true));
    std::vector<std::string> roster;for(int i=0;i<10;++i)roster.push_back("Skyrim.esm|member"+std::to_string(i));
    auto allocation=Allocate(balanced,roster,EligibleIndices(balanced,false));
    require(allocation.size()==10);
    require(std::count_if(allocation.begin(),allocation.end(),[](auto& p){return p.second==0;})==5);
    require(std::count_if(allocation.begin(),allocation.end(),[](auto& p){return p.second==1;})==5);
    std::reverse(roster.begin(),roster.end());require(allocation==Allocate(balanced,roster,EligibleIndices(balanced,true)));
    roster.push_back(roster.front());require(allocation==Allocate(balanced,roster,EligibleIndices(balanced,false)));
    balanced.outfits[0].name="Renamed";require(allocation==Allocate(balanced,roster,EligibleIndices(balanced,false)));
    balanced.outfits[0].pieces={"newArmor"};require(allocation==Allocate(balanced,roster,EligibleIndices(balanced,false)));
    balanced.outfits[0].sex=1;balanced.outfits[1].sex=1;
    balanced.outfits.push_back({"Female",100,2,3,{"femaleArmor"}});
    require(EligibleIndices(balanced,false)==std::vector<int>({0,1}));
    require(EligibleIndices(balanced,true)==std::vector<int>({2}));
    auto females=Allocate(balanced,{"f1","f2","f3"},EligibleIndices(balanced,true));
    require(females.size()==3 && std::all_of(females.begin(),females.end(),[](auto& p){return p.second==2;}));
    require(Allocate(balanced,roster,{}).empty());
    std::cout<<checks<<" rule, pool and faction-quota checks passed\n";
}
