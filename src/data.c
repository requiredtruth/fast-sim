// all game content is text: items, recipes, tuning and the starting kit. the built-in table below
// uses exactly the format mods use, so anything the game ships with can be changed or added to by a mod.
//
//   item   id=beans name="Can of beans" kind=food eat=28 weight=0.4 stack=6 price=6 loot=shelf:6,fridge:3 icon=can color=c8a040
//   recipe id=bandages out=bandage:2 in=rags:2
//   recipe id=boil out=water:1 in=dirty_water:1 fire=1
//   tune   hunger=1.0 thirst=1.0 fuel=1.0 raiders=1.0 loot=1.0 daylen=24
//   start  item=beans:2
//
// a line that names an existing item id changes that item (only the fields given). lines starting with # are comments.
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "game.h"

Item items[MAX_ITEMS]; int nitems;
Recipe recipes[MAX_RECIPES]; int nrecipes;
char data_errors[512];

// global tuning a mod can change
float tune_hunger = 1, tune_thirst = 1, tune_fuel = 1, tune_raiders = 1, tune_loot = 1, tune_cold = 1;
Slot start_kit[16]; int nstart;

static const char *BASE =
"# ---- food\n"
"item id=beans name=\"Can of beans\" kind=food eat=28 water=-2 weight=0.45 stack=8 price=6 icon=can color=c89a50 loot=shelf:7,fridge:2,trunk:2,bag:3,crate:3,desk:1 desc=\"Cold beans. Better than nothing.\"\n"
"item id=soup name=\"Canned soup\" kind=food eat=24 water=8 weight=0.45 stack=8 price=7 icon=can color=c05040 loot=shelf:6,trunk:1,bag:2,crate:2 desc=\"Salty and warm if you heat it.\"\n"
"item id=peaches name=\"Canned peaches\" kind=food eat=18 water=12 weight=0.45 stack=8 price=7 icon=can color=e8a040 loot=shelf:5,fridge:2,bag:1 desc=\"Syrup counts as water, right?\"\n"
"item id=jerky name=\"Beef jerky\" kind=food eat=20 water=-4 weight=0.15 stack=12 price=6 icon=meat color=8a4a2a loot=shelf:5,desk:2,trunk:2,bag:4,corpse:2,locker:1 desc=\"Light, lasts forever, makes you thirsty.\"\n"
"item id=chips name=\"Bag of chips\" kind=food eat=10 water=-3 weight=0.1 stack=10 price=3 icon=box color=e0c040 loot=shelf:8,desk:2,trunk:2,bag:2 desc=\"Mostly air.\"\n"
"item id=candy name=\"Candy bar\" kind=food eat=12 rest=4 weight=0.08 stack=12 price=3 icon=box color=a04030 loot=shelf:7,desk:4,trunk:2,bag:3,corpse:2 desc=\"A little energy.\"\n"
"item id=mre name=\"Ration pack\" kind=food eat=55 water=4 weight=0.6 stack=6 price=18 icon=box color=6a7a4a loot=locker:4,crate:3,corpse:1,trunk:1 desc=\"Military meal. A whole day of food.\"\n"
"item id=crackers name=\"Crackers\" kind=food eat=14 water=-2 weight=0.2 stack=10 price=3 icon=box color=d8c090 loot=shelf:6,bag:2,desk:1 desc=\"Dry, plain, filling enough.\"\n"
"item id=apple name=\"Apple\" kind=food eat=10 water=6 weight=0.2 stack=10 price=2 icon=leaf color=c03028 loot=fridge:5,crate:3,bag:2 desc=\"From an old orchard.\"\n"
"item id=pbutter name=\"Peanut butter\" kind=food eat=40 water=-6 weight=0.5 stack=4 price=10 icon=can color=b07840 loot=shelf:4,fridge:1,trunk:1 desc=\"Dense calories.\"\n"
"item id=raw_meat name=\"Raw venison\" kind=food eat=20 sick=0.45 weight=0.8 stack=6 price=4 icon=meat color=b03030 desc=\"Cook it first, unless you like gambling.\"\n"
"item id=meat name=\"Cooked venison\" kind=food eat=45 weight=0.7 stack=6 price=12 icon=meat color=7a4020 desc=\"Smoky and good.\"\n"
"item id=raw_fish name=\"Raw fish\" kind=food eat=14 sick=0.35 weight=0.5 stack=6 price=3 icon=meat color=8aa0a8 desc=\"Fresh from the pond.\"\n"
"item id=fish name=\"Grilled fish\" kind=food eat=32 water=2 weight=0.4 stack=6 price=9 icon=meat color=c0a070 desc=\"Flaky.\"\n"
"item id=coffee name=\"Instant coffee\" kind=food rest=22 water=4 weight=0.1 stack=10 price=5 icon=can color=4a3020 loot=shelf:3,desk:3,fridge:1 desc=\"Keeps you awake behind the wheel.\"\n"
"# ---- drink\n"
"item id=water name=\"Bottle of water\" kind=drink drink=30 weight=0.55 stack=8 price=5 empty=bottle icon=bottle color=60a8e8 loot=shelf:6,fridge:7,trunk:3,bag:3,locker:1,crate:2 desc=\"Clean water.\"\n"
"item id=jug name=\"Water jug\" kind=drink drink=70 weight=2.1 stack=3 price=12 empty=bottle icon=bottle color=4888c8 loot=shelf:2,fridge:1,trunk:2,crate:1 desc=\"A gallon of water.\"\n"
"item id=dirty_water name=\"Dirty water\" kind=drink drink=30 sick=0.5 weight=0.55 stack=8 price=1 empty=bottle icon=bottle color=8a8a60 desc=\"Boil it at a fire first.\"\n"
"item id=bottle name=\"Empty bottle\" kind=mat weight=0.05 stack=10 price=1 icon=bottle color=b0c0c8 loot=bag:2,trunk:1,desk:1 desc=\"Fill it at a well, a pump or a pond.\"\n"
"item id=soda name=\"Can of soda\" kind=drink drink=18 eat=4 rest=3 weight=0.35 stack=8 price=3 icon=can color=d02020 loot=fridge:8,shelf:4,desk:1,trunk:1 desc=\"Warm and flat.\"\n"
"item id=energy name=\"Energy drink\" kind=drink drink=14 rest=30 weight=0.3 stack=8 price=6 icon=can color=60d040 loot=fridge:4,shelf:3,trunk:1,corpse:1 desc=\"Your heart will thank you later. Maybe.\"\n"
"item id=whiskey name=\"Whiskey\" kind=drink drink=6 warm=25 rest=-8 weight=0.8 stack=4 price=22 icon=bottle color=b06a20 loot=desk:3,shelf:2,locker:1,corpse:1,trunk:1 desc=\"Warms you up. Trades well.\"\n"
"# ---- medicine\n"
"item id=bandage name=\"Bandage\" kind=med bandage=15 weight=0.05 stack=12 price=6 icon=cross color=f0f0f0 loot=shelf:3,desk:2,locker:3,bag:2,corpse:2,trunk:2 desc=\"Stops bleeding.\"\n"
"item id=medkit name=\"First aid kit\" kind=med heal=55 weight=0.6 stack=3 price=40 icon=cross color=e03030 loot=locker:3,shelf:1,trunk:1,desk:1 desc=\"Patches you up properly.\"\n"
"item id=painkiller name=\"Painkillers\" kind=med heal=12 rest=6 weight=0.05 stack=10 price=10 icon=pill color=f0e0a0 loot=desk:3,shelf:2,locker:2,bag:1,corpse:1 desc=\"Takes the edge off.\"\n"
"item id=antibiotic name=\"Antibiotics\" kind=med cure=70 weight=0.05 stack=6 price=35 icon=pill color=80c0ff loot=locker:2,desk:1,shelf:1 desc=\"For when the water was a mistake.\"\n"
"item id=vitamins name=\"Vitamins\" kind=med cure=20 heal=4 weight=0.05 stack=10 price=8 icon=pill color=f0a030 loot=shelf:2,desk:2,bag:1 desc=\"Can't hurt.\"\n"
"# ---- fuel\n"
"item id=gas_can name=\"Jerry can (full)\" kind=fuel fuel=12 weight=10 stack=2 price=40 empty=gas_empty icon=jerry color=c02020 loot=trunk:3,toolbox:2,crate:2,locker:1 desc=\"Twelve litres of gas.\"\n"
"item id=gas_empty name=\"Jerry can (empty)\" kind=tool weight=1 stack=3 price=10 icon=jerry color=703030 loot=trunk:2,toolbox:1,crate:1 desc=\"Fill it at a pump, or siphon a wreck with a hose.\"\n"
"item id=siphon name=\"Siphon hose\" kind=tool weight=0.3 stack=1 price=15 icon=gear color=60a060 loot=toolbox:4,trunk:3,crate:2 desc=\"Drains the tank of an abandoned car into your jerry cans.\"\n"
"# ---- car parts\n"
"item id=tire name=\"Spare tyre\" kind=part tire=100 weight=9 stack=2 price=45 icon=tire color=303030 loot=trunk:3,toolbox:1 desc=\"Swaps out the worst wheel. Needs a jack.\"\n"
"item id=patch name=\"Tyre patch kit\" kind=part tire=45 weight=0.3 stack=4 price=14 icon=tire color=806040 loot=toolbox:4,trunk:2,shelf:1 desc=\"Plugs a puncture.\"\n"
"item id=battery name=\"Car battery\" kind=part battery=100 weight=12 stack=1 price=50 icon=battery color=3060a0 loot=toolbox:2,trunk:1 desc=\"Twelve volts of hope.\"\n"
"item id=coolant name=\"Coolant\" kind=part radiator=45 weight=2 stack=3 price=16 icon=bottle color=40d060 loot=toolbox:3,shelf:2,trunk:2 desc=\"Keeps the engine from cooking.\"\n"
"item id=engine_parts name=\"Engine parts\" kind=part engine=40 weight=4 stack=3 price=55 icon=gear color=8a8a8a loot=toolbox:3,trunk:1,crate:1 desc=\"Belts, gaskets, a pump. Needs a toolkit.\"\n"
"item id=plugs name=\"Spark plugs\" kind=part engine=15 weight=0.2 stack=6 price=15 icon=gear color=c0c0c0 loot=toolbox:4,shelf:1,trunk:1 desc=\"Smooths out a rough idle.\"\n"
"item id=oil name=\"Motor oil\" kind=part engine=10 weight=1 stack=4 price=10 icon=bottle color=c0a020 loot=toolbox:3,shelf:3,trunk:2 desc=\"A top-up for a thirsty engine.\"\n"
"item id=armor_kit name=\"Armour plating\" kind=part armor=1 weight=14 stack=1 price=80 icon=gear color=5a5a5a desc=\"Bolt-on plates. Cuts crash and bullet damage.\"\n"
"# ---- tools\n"
"item id=toolkit name=\"Toolkit\" kind=tool weight=3 stack=1 price=40 icon=wrench color=c03020 loot=toolbox:5,trunk:2,crate:1 desc=\"Needed for engine work and crafting armour.\"\n"
"item id=jack name=\"Jack and tyre iron\" kind=tool weight=4 stack=1 price=25 icon=wrench color=505860 loot=trunk:4,toolbox:2 desc=\"Needed to change a tyre.\"\n"
"item id=lighter name=\"Lighter\" kind=tool weight=0.05 stack=1 price=8 icon=flame color=e0a020 loot=desk:3,shelf:2,bag:2,corpse:2,trunk:1 desc=\"Starts fires. Never runs out here.\"\n"
"item id=matches name=\"Matches\" kind=mat weight=0.05 stack=20 price=2 icon=flame color=d08040 loot=shelf:3,desk:2,bag:2,crate:1 desc=\"One match lights one fire.\"\n"
"item id=flashlight name=\"Flashlight\" kind=gear light=1 weight=0.4 stack=1 price=20 icon=gear color=e8e8c0 loot=desk:2,toolbox:2,trunk:2,locker:1,bag:1 desc=\"Use it to switch it on and off.\"\n"
"item id=batteries name=\"AA batteries\" kind=gear light=400 weight=0.1 stack=8 price=6 icon=battery color=d0b040 loot=desk:3,shelf:3,toolbox:1,bag:1 desc=\"Recharges the flashlight.\"\n"
"item id=rod name=\"Fishing rod\" kind=tool weight=1.2 stack=1 price=25 icon=gear color=8a6a3a loot=crate:2,trunk:1,locker:1 desc=\"Use it next to a pond to fish.\"\n"
"# ---- ammunition and weapons\n"
"item id=ammo_38 name=\".38 rounds\" kind=ammo ammo=pistol weight=0.01 stack=60 price=1 icon=bullet color=c0a040 loot=locker:5,desk:2,corpse:5,trunk:2,bag:2 desc=\"Revolver ammunition.\"\n"
"item id=ammo_rifle name=\"Rifle cartridges\" kind=ammo ammo=rifle weight=0.02 stack=40 price=2 icon=bullet color=c08040 loot=locker:4,crate:2,corpse:2,trunk:1 desc=\"For the hunting rifle.\"\n"
"item id=shells name=\"Shotgun shells\" kind=ammo ammo=shotgun weight=0.04 stack=30 price=2 icon=bullet color=c03020 loot=locker:4,crate:2,corpse:3,trunk:2,farm:2 desc=\"Twelve gauge.\"\n"
"item id=revolver name=\"Revolver\" kind=weapon weapon=pistol weight=1.1 stack=1 price=90 icon=gun color=404448 loot=locker:2,desk:1,corpse:1 desc=\"Six shots. Make them count.\"\n"
"item id=rifle name=\"Hunting rifle\" kind=weapon weapon=rifle weight=3.6 stack=1 price=140 icon=gun color=6a4424 loot=locker:2,crate:1 desc=\"Long range. Good for deer.\"\n"
"item id=shotgun name=\"Shotgun\" kind=weapon weapon=shotgun weight=3.2 stack=1 price=120 icon=gun color=2a2c30 loot=locker:2,trunk:1,corpse:1 desc=\"Two barrels, close range.\"\n"
"item id=pipe name=\"Lead pipe\" kind=weapon weapon=bat weight=1.5 stack=1 price=10 icon=gun color=7a7c80 loot=toolbox:2,crate:2,trunk:2,bag:1 desc=\"Quiet. Hits hard.\"\n"
"# ---- gear\n"
"item id=backpack name=\"Hiking backpack\" kind=gear weight=1.2 stack=1 price=45 icon=bag color=4a6a3a loot=locker:2,bag:3,trunk:1 desc=\"Carry 15 kg more while you have it.\"\n"
"item id=jacket name=\"Warm jacket\" kind=gear weight=1.5 stack=1 price=35 icon=bag color=6a5040 loot=locker:3,bag:2,trunk:2 desc=\"Halves how fast the cold gets to you.\"\n"
"item id=sleeping_bag name=\"Sleeping bag\" kind=gear weight=1.8 stack=1 price=30 icon=bag color=3a5a8a loot=locker:2,bag:2,trunk:2,crate:1 desc=\"Sleep warm anywhere.\"\n"
"item id=tent name=\"Tent\" kind=gear tent=1 weight=3 stack=1 price=40 icon=bag color=3a6a8a loot=trunk:2,crate:2,bag:1 desc=\"Use it to pitch camp. Safer nights.\"\n"
"# ---- materials\n"
"item id=rags name=\"Rags\" kind=mat weight=0.1 stack=20 price=1 icon=bag color=c0b8a0 loot=shelf:2,desk:2,bag:3,trunk:2,corpse:2,crate:1 desc=\"Bandages, wicks, cleaning.\"\n"
"item id=wood name=\"Firewood\" kind=mat weight=1.5 stack=10 price=2 icon=plank color=8a6038 loot=crate:4,trunk:1,bag:1 desc=\"Two make a campfire.\"\n"
"item id=scrap name=\"Scrap metal\" kind=mat weight=1 stack=20 price=3 icon=gear color=8a8a90 loot=toolbox:3,trunk:3,crate:3,bag:1 desc=\"Raw material for repairs.\"\n"
"item id=tape name=\"Duct tape\" kind=mat weight=0.2 stack=10 price=5 icon=box color=a0a0a8 loot=toolbox:4,shelf:2,trunk:2,desk:1,bag:1 desc=\"Fixes nearly anything for a while.\"\n"
"item id=rope name=\"Rope\" kind=mat weight=0.6 stack=5 price=5 icon=bag color=b09060 loot=crate:2,trunk:2,toolbox:1 desc=\"Useful for lashing things together.\"\n"
"# ---- trade\n"
"item id=cash name=\"Dollars\" kind=trade weight=0 stack=9999 price=1 icon=coin color=60b060 loot=desk:6,corpse:5,bag:3,locker:2,trunk:2,bed:2 desc=\"Traders still take it.\"\n"
"item id=cigs name=\"Cigarettes\" kind=trade weight=0.05 stack=20 price=8 icon=box color=e0e0e0 loot=desk:3,shelf:2,corpse:3,trunk:1 desc=\"Good for trading.\"\n"
"item id=watch name=\"Gold watch\" kind=trade weight=0.1 stack=5 price=60 icon=coin color=e0b030 loot=desk:1,bed:2,corpse:1,locker:1 desc=\"Somebody's heirloom.\"\n"
"item id=oats name=\"Rolled oats\" kind=food eat=20 water=-3 weight=0.3 stack=8 price=5 icon=box color=d5b076 loot=shelf:5,crate:2,bag:2 desc=\"Dry oats for a trail breakfast.\"\n"
"item id=rice name=\"Bag of rice\" kind=food eat=18 water=-4 weight=0.45 stack=6 price=6 icon=bag color=dad3a3 loot=shelf:5,crate:3 desc=\"A pantry staple. Cook it with water.\"\n"
"item id=trail_mix name=\"Trail mix\" kind=food eat=25 rest=6 weight=0.22 stack=8 price=8 icon=bag color=c99663 loot=bag:4,shelf:3,trunk:1 desc=\"Nuts and dried fruit for the road.\"\n"
"item id=nuts name=\"Roasted almonds\" kind=food eat=18 water=-2 weight=0.15 stack=8 price=7 icon=bag color=a87850 loot=shelf:4,bag:4,desk:2 desc=\"A handful goes a long way.\"\n"
"item id=raisins name=\"Box of raisins\" kind=food eat=12 rest=3 weight=0.13 stack=10 price=4 icon=box color=a25855 loot=shelf:5,bag:3,desk:2 desc=\"Sticky road food with a long shelf life.\"\n"
"item id=rice_pot name=\"Cooked rice\" kind=food eat=40 water=8 weight=0.5 stack=5 price=13 icon=can color=d4bc94 loot=fridge:1 desc=\"A warm bowl of rice.\"\n"
"item id=fish_stew name=\"Fish stew\" kind=food eat=44 water=10 warm=9 weight=0.48 stack=5 price=16 icon=can color=8a9460 loot=fridge:1 desc=\"Hot fish and broth.\"\n"
"item id=corn name=\"Canned corn\" kind=food eat=23 water=4 weight=0.43 stack=8 price=6 icon=can color=f0c85b loot=shelf:6,crate:3,trunk:1 desc=\"Sweet corn in a pull-tab can.\"\n"
"item id=tea name=\"Bottled tea\" kind=drink drink=26 rest=7 weight=0.48 stack=8 price=5 icon=bottle color=b68047 loot=fridge:5,shelf:3,desk:2 desc=\"A little caffeine and water.\"\n"
"item id=tonic name=\"Ginger tonic\" kind=med cure=27 heal=3 weight=0.31 stack=5 price=12 icon=bottle color=8fba66 loot=shelf:2,bag:2,locker:2 desc=\"Settles an upset stomach.\"\n"
"item id=gauze name=\"Sterile gauze\" kind=med bandage=12 weight=0.05 stack=10 price=5 icon=cross color=d9d6c8 loot=locker:4,desk:2,bag:2 desc=\"A clean pad for bleeding wounds.\"\n"
"item id=salve name=\"Healing salve\" kind=med heal=18 weight=0.14 stack=5 price=12 icon=pill color=7bac82 loot=locker:3,bag:2,shelf:2 desc=\"Treats minor wounds.\"\n"
"item id=radiator_hose name=\"Radiator hose\" kind=part radiator=24 weight=0.6 stack=4 price=14 icon=gear color=576659 loot=toolbox:4,trunk:2,crate:1 desc=\"Replaces cracked coolant tubing.\"\n"
"item id=drive_belt name=\"Drive belt\" kind=part engine=20 weight=0.42 stack=4 price=18 icon=tire color=7b664e loot=toolbox:4,trunk:2 desc=\"A fresh belt for a tired motor.\"\n"
"item id=spark_lead name=\"Ignition lead\" kind=part engine=12 weight=0.21 stack=6 price=9 icon=gear color=7391b0 loot=toolbox:5,trunk:1,crate:2 desc=\"A replacement ignition cable.\"\n"
"item id=wiring name=\"Copper wire\" kind=mat weight=0.26 stack=12 price=5 icon=gear color=c68047 loot=toolbox:4,desk:1,crate:3 desc=\"Wire for improvised repairs.\"\n"
"item id=rubber_strip name=\"Rubber strip\" kind=mat weight=0.17 stack=12 price=3 icon=bag color=65625d loot=toolbox:3,trunk:2,crate:3 desc=\"A strip of salvageable rubber.\"\n"
"item id=resin name=\"Pine resin\" kind=mat weight=0.12 stack=10 price=4 icon=can color=bb823c loot=bag:2,crate:3,shelf:1 desc=\"Sticky resin for patching and sealing.\"\n"
"item id=canvas name=\"Canvas roll\" kind=mat weight=0.55 stack=6 price=8 icon=bag color=aca37c loot=crate:3,locker:3,trunk:2 desc=\"Heavy cloth for camp gear.\"\n"
"item id=metal_sheet name=\"Aluminum sheet\" kind=mat weight=0.78 stack=8 price=9 icon=plank color=b5bbc1 loot=toolbox:3,crate:3,trunk:1 desc=\"Sheet stock for repairs.\"\n"
"item id=postcards name=\"Old postcards\" kind=trade weight=0.05 stack=20 price=7 icon=box color=cf977e loot=desk:4,bed:3,bag:1 desc=\"Roadside souvenirs worth a few dollars.\"\n"
"item id=brass_badge name=\"Brass badge\" kind=trade weight=0.10 stack=5 price=27 icon=coin color=bd9854 loot=desk:1,locker:2,crate:1 desc=\"Collectors pay for these.\"\n"
"item id=floor_panel name=\"Timber floor panel\" kind=mat weight=0.7 stack=8 price=8 icon=plank color=a67848 loot=crate:1\n"
"item id=wall_panel name=\"Timber wall panel\" kind=mat weight=0.7 stack=8 price=8 icon=plank color=a67848 loot=crate:1\n"
"item id=door_frame name=\"Timber doorway\" kind=mat weight=0.7 stack=8 price=8 icon=plank color=986030 loot=crate:1\n"
"item id=wood_door name=\"Timber door\" kind=mat weight=0.7 stack=8 price=8 icon=plank color=805020 loot=crate:1\n"
"item id=roof_panel name=\"Timber roof panel\" kind=mat weight=0.7 stack=8 price=8 icon=plank color=a06848 loot=crate:1\n"
"item id=soil_plot name=\"Soil plot kit\" kind=mat weight=0.7 stack=8 price=8 icon=bag color=65482f loot=crate:2\n"
#define GARMENT(id, name, color, group, style) "item id=" #id " name=\"" name "\" kind=gear weight=.45 stack=1 price=12 icon=bag color=" #color " loot=locker:2,crate:1\n"
#define CROP(seed, crop, name, color, style, days) \
    "item id=" #seed " name=\"" name " seeds\" kind=mat weight=.04 stack=20 price=3 icon=box color=" #color " loot=crate:2,shelf:1\n" \
    "item id=" #crop " name=\"" name "\" kind=" CROP_KIND_##style " weight=.2 stack=12 price=5 icon=leaf color=" #color "\n"
#define CAR_PART(id, name, effect, shape) "item id=" #id " name=\"" name "\" kind=part " effect " weight=1.2 stack=4 price=18 icon=gear color=84939b loot=toolbox:2,trunk:1\n"
#define CROP_KIND_0 "food eat=14 water=4"
#define CROP_KIND_1 "food eat=14 water=4"
#define CROP_KIND_2 "food eat=14 water=4"
#define CROP_KIND_3 "food eat=14 water=4"
#define CROP_KIND_4 "food eat=14 water=4"
#define CROP_KIND_5 "food eat=14 water=4"
#define CROP_KIND_6 "food eat=14 water=4"
#define CROP_KIND_7 "food eat=14 water=4"
#define CROP_KIND_8 "food eat=14 water=4"
#define CROP_KIND_9 "food eat=14 water=4"
#define CROP_KIND_10 "food eat=14 water=4"
#define CROP_KIND_11 "food eat=14 water=4"
#define CROP_KIND_12 "food eat=14 water=4"
#define CROP_KIND_13 "food eat=14 water=4"
#define CROP_KIND_14 "food eat=14 water=4"
#define CROP_KIND_15 "trade"
#include "catalog.def"
#undef GARMENT
#undef CROP
#undef CAR_PART
"item id=garden_hoe name=\"Garden hoe\" kind=tool weight=1.5 stack=1 price=15 icon=wrench color=857752 loot=toolbox:2,crate:3 desc=\"Use on dry ground to till a garden patch.\"\n"
"item id=watering_can name=\"Watering can\" kind=tool weight=.5 stack=1 price=9 icon=jerry color=71968b loot=crate:3 desc=\"Water a planted plot with a bottle from the pack.\"\n"
"item id=compost name=\"Garden compost\" kind=mat weight=.4 stack=8 price=5 icon=bag color=735039 loot=crate:2 desc=\"Speeds up the growth of a watered plant.\"\n"
"item id=hand_trowel name=\"Hand trowel\" kind=tool weight=.4 stack=1 price=8 icon=wrench color=737468 loot=toolbox:3,crate:2 desc=\"Use on dry ground to till a garden patch.\"\n"
"# ---- crafting\n"
"recipe id=bandages out=bandage:2 in=rags:2\n"
"recipe id=boil_water out=water:1 in=dirty_water:1 fire=1\n"
"recipe id=cook_meat out=meat:1 in=raw_meat:1 fire=1\n"
"recipe id=cook_fish out=fish:1 in=raw_fish:1 fire=1\n"
"recipe id=patch_kit out=patch:1 in=tape:1,rags:1\n"
"recipe id=armor out=armor_kit:1 in=scrap:8,tape:2 tool=toolkit\n"
"recipe id=rebuild out=engine_parts:1 in=scrap:4,tape:1,oil:1 tool=toolkit\n"
"recipe id=firstaid out=medkit:1 in=bandage:3,painkiller:1\n"
"recipe id=siphon_hose out=siphon:1 in=rope:1,tape:1\n"
"recipe id=salvage out=scrap:2 in=gas_empty:1\n"
"recipe id=trail_mix out=trail_mix:2 in=nuts:1,raisins:1\n"
"recipe id=rice_pot out=rice_pot:2 in=rice:1,water:1 fire=1\n"
"recipe id=fish_stew out=fish_stew:1 in=raw_fish:1,water:1,corn:1 fire=1\n"
"recipe id=resin_patch out=patch:1 in=resin:1,rubber_strip:1\n"
"recipe id=gauze_wrap out=bandage:2 in=gauze:1,rags:1\n"
"recipe id=build_tent out=tent:1 in=canvas:2,rope:1,metal_sheet:1\n"
"recipe id=make_lead out=spark_lead:1 in=wiring:2,rubber_strip:1 tool=toolkit\n"
"recipe id=sheet_scrap out=scrap:3 in=metal_sheet:1 tool=toolkit\n"
"recipe id=floor_panel out=floor_panel:1 in=wood:2\n"
"recipe id=wall_panel out=wall_panel:1 in=wood:2\n"
"recipe id=door_frame out=door_frame:1 in=wood:2\n"
"recipe id=wood_door out=wood_door:1 in=wood:2\n"
"recipe id=roof_panel out=roof_panel:1 in=wood:2\n"
"recipe id=soil_plot out=soil_plot:1 in=wood:1,rags:1\n"
"recipe id=garden_hoe out=garden_hoe:1 in=wood:1,scrap:1\n"
"recipe id=watering_can out=watering_can:1 in=metal_sheet:1,tape:1\n"
"recipe id=compost out=compost:2 in=garden_beans:1,wood:1\n"
"recipe id=hand_trowel out=hand_trowel:1 in=wood:1,scrap:1\n"
"# ---- starting kit\n"
"start item=water:2 item=beans:2 item=bandage:2 item=lighter:1 item=gas_empty:1 item=siphon:1 item=jack:1 item=cash:30 item=pipe:1 item=rags:2 item=soil_plot:1 item=bean_seed:2 item=wood:6 item=garden_hoe:1 item=carrot_seed:2\n";

// the example a player sees in the mods folder, disabled until they turn it on
const char *EXAMPLE_MOD =
"# Example mod for Long Road. Copy this file, change it, and switch it on in Mods.\n"
"# Each line is one thing. See the README for every field.\n"
"mod name=\"Desert Larder\" author=\"you\"\n"
"item id=prickly_pear name=\"Prickly pear\" kind=food eat=8 water=14 weight=0.2 stack=10 price=2 icon=leaf color=c04080 loot=bag:3,crate:2 desc=\"Spines removed. Mostly.\"\n"
"item id=canteen name=\"Canteen\" kind=drink drink=45 weight=0.9 stack=2 price=15 empty=bottle icon=bottle color=6a7a4a loot=locker:2,trunk:1 desc=\"Holds more than a bottle.\"\n"
"recipe id=pear_juice out=water:1 in=prickly_pear:2\n"
"tune hunger=0.9\n";

static const char *kind_names[IK_COUNT] = { "food", "drink", "med", "fuel", "part", "tool", "ammo", "weapon", "gear", "mat", "trade" };
static const char *icon_names[ICON_COUNT] = { "can", "bottle", "box", "cross", "jerry", "gear", "wrench", "bullet", "gun", "bag",
                                              "plank", "coin", "tire", "battery", "pill", "flame", "meat", "leaf" };
static const char *ct_names[CT_COUNT] = { "shelf", "fridge", "crate", "locker", "trunk", "desk", "toolbox", "bag", "pump", "well",
                                          "bed", "bench", "trader", "corpse" };
static const char *wnames[W_COUNT] = { "fist", "pistol", "rifle", "shotgun", "bat" };

const char *item_kind_name(int k) { return k >= 0 && k < IK_COUNT ? kind_names[k] : "?"; }

int item_find(const char *id)
{
    for (int i = 0; i < nitems; i++) if (!strcmp(items[i].id, id)) return i;
    return -1;
}

static int find_name(const char *const *tab, int n, const char *v)
{
    for (int i = 0; i < n; i++) if (!strcmp(tab[i], v)) return i;
    return -1;
}

static void err(const char *src, int line, const char *msg)
{
    size_t l = strlen(data_errors);
    if (l < sizeof data_errors - 80) snprintf(data_errors + l, sizeof data_errors - l, "%s:%d %s\n", src, line, msg);
}

// split "key=value" pairs; values may be quoted
typedef struct { char k[16], v[96]; } KV;

static int parse_kv(const char *s, KV *out, int max)
{
    int n = 0;
    while (*s && n < max) {
        while (*s == ' ' || *s == '\t') s++;
        if (!*s || *s == '#') break;
        int kl = 0;
        while (*s && *s != '=' && *s != ' ' && kl < 15) out[n].k[kl++] = (char)tolower((unsigned char)*s++);
        out[n].k[kl] = 0;
        int vl = 0;
        if (*s == '=') {
            s++;
            if (*s == '"') { s++; while (*s && *s != '"' && vl < 95) out[n].v[vl++] = *s++; if (*s == '"') s++; }
            else while (*s && *s != ' ' && *s != '\t' && vl < 95) out[n].v[vl++] = *s++;
        }
        out[n].v[vl] = 0;
        while (*s && *s != ' ' && *s != '\t') s++;
        n++;
    }
    return n;
}

static bool parse_pair(const char *v, char *id, int *n)
{
    const char *c = strchr(v, ':');
    if (!c) { snprintf(id, 24, "%s", v); *n = 1; return true; }
    int l = (int)(c - v);
    if (l > 23) l = 23;
    memcpy(id, v, l); id[l] = 0;
    *n = atoi(c + 1);
    return *n > 0;
}

static void item_line(KV *kv, int n, const char *src, int line, bool mod)
{
    const char *id = NULL;
    for (int i = 0; i < n; i++) if (!strcmp(kv[i].k, "id")) id = kv[i].v;
    if (!id || !*id) { err(src, line, "item without id"); return; }
    int it = item_find(id);
    if (it < 0) {
        if (nitems >= MAX_ITEMS) { err(src, line, "too many items"); return; }
        it = nitems++;
        memset(&items[it], 0, sizeof items[it]);
        snprintf(items[it].id, sizeof items[it].id, "%s", id);
        snprintf(items[it].name, sizeof items[it].name, "%s", id);
        items[it].weight = 0.5f; items[it].stack = 5; items[it].price = 5; items[it].weapon = -1;
        items[it].color = 0xc0c0c0; items[it].icon = ICON_BOX;
        items[it].mod = mod;
    }
    Item *t = &items[it];
    for (int i = 0; i < n; i++) {
        const char *k = kv[i].k, *v = kv[i].v;
        float f = (float)atof(v);
        if (!strcmp(k, "id")) continue;
        else if (!strcmp(k, "name")) snprintf(t->name, sizeof t->name, "%s", v);
        else if (!strcmp(k, "desc")) snprintf(t->desc, sizeof t->desc, "%s", v);
        else if (!strcmp(k, "kind")) { int x = find_name(kind_names, IK_COUNT, v); if (x < 0) err(src, line, "unknown kind"); else t->kind = x; }
        else if (!strcmp(k, "weight")) t->weight = f;
        else if (!strcmp(k, "stack")) t->stack = (int)Clamp(f, 1, 9999);
        else if (!strcmp(k, "price")) t->price = (int)f;
        else if (!strcmp(k, "sick")) t->sick = Clamp(f, 0, 1);
        else if (!strcmp(k, "color")) t->color = (int)strtol(v, NULL, 16);
        else if (!strcmp(k, "icon")) { int x = find_name(icon_names, ICON_COUNT, v); if (x >= 0) t->icon = x; }
        else if (!strcmp(k, "empty")) continue;   // item_extras
        else if (!strcmp(k, "eat")) { if (t->effect == E_DRINK) t->value2 = f; else { t->effect = E_EAT; t->value = f; } }
        else if (!strcmp(k, "drink")) { if (t->effect == E_EAT) t->value2 = f; else { t->effect = E_DRINK; t->value = f; } }
        else if (!strcmp(k, "water")) t->value2 = f;
        else if (!strcmp(k, "heal")) { if (t->effect == E_NONE || t->effect == E_HEAL) { t->effect = E_HEAL; t->value = f; } }
        else if (!strcmp(k, "bandage")) { t->effect = E_BANDAGE; t->value = f; }
        else if (!strcmp(k, "rest")) { if (t->effect == E_NONE || t->effect == E_REST) { t->effect = E_REST; t->value = f; } }
        else if (!strcmp(k, "warm")) { if (t->effect == E_NONE || t->effect == E_WARM) { t->effect = E_WARM; t->value = f; } }
        else if (!strcmp(k, "fuel")) { t->effect = E_FUEL; t->value = f; }
        else if (!strcmp(k, "engine")) { t->effect = E_ENGINE; t->value = f; }
        else if (!strcmp(k, "tire")) { t->effect = E_TIRE; t->value = f; }
        else if (!strcmp(k, "battery")) { t->effect = E_BATTERY; t->value = f; }
        else if (!strcmp(k, "radiator")) { t->effect = E_RADIATOR; t->value = f; }
        else if (!strcmp(k, "armor")) { t->effect = E_ARMOR; t->value = f; }
        else if (!strcmp(k, "light")) { t->effect = E_LIGHT; t->value = f; }
        else if (!strcmp(k, "cure")) { t->effect = E_CURE; t->value = f; }
        else if (!strcmp(k, "tent")) { t->effect = E_TENT; t->value = f; }
        else if (!strcmp(k, "camp")) { t->effect = E_CAMP; t->value = f; }
        else if (!strcmp(k, "weapon")) { int x = find_name(wnames, W_COUNT, v); if (x < 0) err(src, line, "unknown weapon"); else { t->effect = E_WEAPON; t->weapon = x; } }
        else if (!strcmp(k, "ammo")) { int x = find_name(wnames, W_COUNT, v); if (x < 0) err(src, line, "unknown ammo"); else t->weapon = x; }
        else if (!strcmp(k, "loot")) {
            char buf[96];
            snprintf(buf, sizeof buf, "%s", v);
            for (char *tok = strtok(buf, ","); tok; tok = strtok(NULL, ",")) {
                char cid[24]; int w = 1;
                parse_pair(tok, cid, &w);
                int ct = find_name(ct_names, CT_COUNT, cid);
                if (ct < 0) {
                    // stop names add to every container at that kind of place; handled as a light general bonus
                    if (!strcmp(cid, "farm") || !strcmp(cid, "gas") || !strcmp(cid, "town")) { t->loot[CT_CRATE] += w * 0.5f; continue; }
                    err(src, line, "unknown loot place");
                    continue;
                }
                t->loot[ct] = (float)w;
            }
        } else err(src, line, k);
    }
}

// what an item turns into when used up (jerry can -> empty can, bottle of water -> empty bottle)
static char empties[MAX_ITEMS][24];
int item_empty(int it)
{
    if (it < 0 || it >= nitems || !empties[it][0]) return -1;
    return item_find(empties[it]);
}
// secondary stats that don't fit the single effect: rest from coffee/candy, heal from vitamins
static float extra_rest[MAX_ITEMS], extra_heal[MAX_ITEMS], extra_warm[MAX_ITEMS];
float item_extra_rest(int it) { return it >= 0 && it < MAX_ITEMS ? extra_rest[it] : 0; }
float item_extra_heal(int it) { return it >= 0 && it < MAX_ITEMS ? extra_heal[it] : 0; }
float item_extra_warm(int it) { return it >= 0 && it < MAX_ITEMS ? extra_warm[it] : 0; }

static void item_extras(KV *kv, int n)
{
    const char *id = NULL;
    for (int i = 0; i < n; i++) if (!strcmp(kv[i].k, "id")) id = kv[i].v;
    int it = id ? item_find(id) : -1;
    if (it < 0) return;
    for (int i = 0; i < n; i++) {
        float f = (float)atof(kv[i].v);
        if (!strcmp(kv[i].k, "empty")) snprintf(empties[it], 24, "%s", kv[i].v);
        if (!strcmp(kv[i].k, "rest") && items[it].effect != E_REST) extra_rest[it] = f;
        if (!strcmp(kv[i].k, "heal") && items[it].effect != E_HEAL) extra_heal[it] = f;
        if (!strcmp(kv[i].k, "warm") && items[it].effect != E_WARM) extra_warm[it] = f;
    }
}

static void recipe_line(KV *kv, int n, const char *src, int line)
{
    Recipe r = { .tool = -1, .out = -1 };
    for (int i = 0; i < n; i++) {
        const char *k = kv[i].k, *v = kv[i].v;
        if (!strcmp(k, "id")) snprintf(r.id, sizeof r.id, "%s", v);
        else if (!strcmp(k, "out")) {
            char id[24]; int c;
            parse_pair(v, id, &c);
            r.out = item_find(id); r.out_n = c;
            if (r.out < 0) { err(src, line, "recipe makes an unknown item"); return; }
        } else if (!strcmp(k, "in")) {
            char buf[96];
            snprintf(buf, sizeof buf, "%s", v);
            for (char *tok = strtok(buf, ","); tok && r.nin < 4; tok = strtok(NULL, ",")) {
                char id[24]; int c;
                parse_pair(tok, id, &c);
                int it = item_find(id);
                if (it < 0) { err(src, line, "recipe uses an unknown item"); return; }
                r.in[r.nin] = it; r.in_n[r.nin] = c; r.nin++;
            }
        } else if (!strcmp(k, "tool")) {
            r.tool = item_find(v);
            if (r.tool < 0) { err(src, line, "unknown tool"); return; }
        } else if (!strcmp(k, "fire")) r.fire = atoi(v) != 0;
    }
    if (r.out < 0 || r.nin == 0) { err(src, line, "recipe needs out= and in="); return; }
    if (!r.id[0]) snprintf(r.id, sizeof r.id, "%s", items[r.out].id);
    for (int i = 0; i < nrecipes; i++) if (!strcmp(recipes[i].id, r.id)) { recipes[i] = r; return; }
    if (nrecipes < MAX_RECIPES) recipes[nrecipes++] = r;
}

static void tune_line(KV *kv, int n)
{
    for (int i = 0; i < n; i++) {
        float f = Clamp((float)atof(kv[i].v), 0, 10);
        const char *k = kv[i].k;
        if (!strcmp(k, "hunger")) tune_hunger = f;
        else if (!strcmp(k, "thirst")) tune_thirst = f;
        else if (!strcmp(k, "fuel")) tune_fuel = f;
        else if (!strcmp(k, "raiders")) tune_raiders = f;
        else if (!strcmp(k, "loot")) tune_loot = f;
        else if (!strcmp(k, "cold")) tune_cold = f;
    }
}

static void start_line(KV *kv, int n, const char *src, int line)
{
    for (int i = 0; i < n; i++) {
        if (strcmp(kv[i].k, "item")) continue;
        char id[24]; int c;
        parse_pair(kv[i].v, id, &c);
        int it = item_find(id);
        if (it < 0) { err(src, line, "start kit has an unknown item"); continue; }
        int k = 0;
        while (k < nstart && start_kit[k].item != it) k++;
        if (k == nstart) { if (nstart >= 16) continue; nstart++; start_kit[k].item = it; start_kit[k].n = 0; }
        start_kit[k].n = c;
    }
}

static void load_text(const char *text, const char *src, bool mod, char *name_out, int name_n)
{
    int line = 0;
    const char *p = text;
    char buf[1024];
    // two passes: items first so recipes and the kit can refer to items defined later in the file
    for (int pass = 0; pass < 2; pass++) {
        p = text; line = 0;
        while (*p) {
            int l = 0;
            while (*p && *p != '\n' && l < 1023) buf[l++] = *p++;
            while (*p && *p != '\n') p++;
            if (*p == '\n') p++;
            buf[l] = 0;
            if (l && buf[l - 1] == '\r') buf[l - 1] = 0;
            line++;
            char *s = buf;
            while (*s == ' ' || *s == '\t') s++;
            if (!*s || *s == '#') continue;
            char type[12] = { 0 };
            int tl = 0;
            while (*s && *s != ' ' && tl < 11) type[tl++] = (char)tolower((unsigned char)*s++);
            KV kv[32];
            int n = parse_kv(s, kv, 32);
            if (pass == 0) {
                if (!strcmp(type, "item")) { item_line(kv, n, src, line, mod); item_extras(kv, n); }
                else if (!strcmp(type, "mod") && name_out) {
                    for (int i = 0; i < n; i++) if (!strcmp(kv[i].k, "name")) snprintf(name_out, name_n, "%s", kv[i].v);
                }
            } else {
                if (!strcmp(type, "recipe")) recipe_line(kv, n, src, line);
                else if (!strcmp(type, "tune")) tune_line(kv, n);
                else if (!strcmp(type, "start")) start_line(kv, n, src, line);
                else if (strcmp(type, "item") && strcmp(type, "mod")) err(src, line, "unknown line type");
            }
        }
    }
}

// ---------------------------------------------------------------- mods

#define MAX_MODS 32
typedef struct { char file[128], name[48]; char *text; bool on; } Mod;
static Mod mods[MAX_MODS];
static int nmods;
static char mods_path[256];

int mods_count(void) { return nmods; }
const char *mod_name(int i) { return i >= 0 && i < nmods ? mods[i].name : ""; }
bool mod_enabled(int i) { return i >= 0 && i < nmods && mods[i].on; }
const char *mods_dir(void) { return mods_path; }

// which mods are on is kept as a list of file names in the save folder
static void mods_state_save(void)
{
    char buf[4096] = { 0 };
    for (int i = 0; i < nmods; i++)
        if (mods[i].on) { strncat(buf, mods[i].file, sizeof buf - strlen(buf) - 2); strcat(buf, "\n"); }
    plat_put("mods_on", buf);
}

static bool mod_on_listed(const char *file)
{
    char *t = plat_get("mods_on");
    if (!t) return false;
    bool on = false;
    for (char *l = strtok(t, "\n"); l; l = strtok(NULL, "\n")) if (!strcmp(l, file)) on = true;
    free(t);
    return on;
}

void mod_toggle(int i)
{
    if (i < 0 || i >= nmods) return;
    mods[i].on = !mods[i].on;
    mods_state_save();
}

static void mod_add(const char *file, const char *text)
{
    for (int i = 0; i < nmods; i++) if (!strcmp(mods[i].file, file)) { free(mods[i].text); mods[i].text = strdup(text); return; }
    if (nmods >= MAX_MODS) return;
    Mod *m = &mods[nmods++];
    snprintf(m->file, sizeof m->file, "%s", file);
    snprintf(m->name, sizeof m->name, "%s", file);
    m->text = strdup(text);
    m->on = mod_on_listed(file);
    // the name comes from a "mod name=..." line if there is one
    const char *nl = strstr(text, "mod name=\"");
    if (nl) {
        nl += 10;
        int k = 0;
        while (nl[k] && nl[k] != '"' && k < 47) { m->name[k] = nl[k]; k++; }
        m->name[k] = 0;
    }
}

#ifdef PLATFORM_WEB
#include <emscripten.h>
EM_JS(char *, web_mods_list, (void), {
    var out = "";
    try { var j = JSON.parse(localStorage.getItem("longroad_mods") || "{}"); for (var k in j) out += k + "\u0001" + j[k] + "\u0002"; } catch (e) {}
    return stringToNewUTF8(out);
});
EM_JS(void, web_mods_put, (const char *name, const char *text), {
    try { var j = JSON.parse(localStorage.getItem("longroad_mods") || "{}"); j[UTF8ToString(name)] = UTF8ToString(text);
          localStorage.setItem("longroad_mods", JSON.stringify(j)); } catch (e) {}
});
#endif

int mod_import_text(const char *name, const char *text)
{
    if (!name || !text) return 1;
    char file[128];
    snprintf(file, sizeof file, "%s", name);
#ifdef PLATFORM_WEB
    web_mods_put(file, text);
#else
    char path[400];
    MakeDirectory(mods_path);
    snprintf(path, sizeof path, "%s/%s", mods_path, file);
    write_text(path, text);
#endif
    mod_add(file, text);
    for (int i = 0; i < nmods; i++) if (!strcmp(mods[i].file, file) && !mods[i].on) mod_toggle(i);
    hud_msg("Mod added. It takes effect on the next trip.", 3);
    return 0;
}

#ifdef PLATFORM_WEB
EMSCRIPTEN_KEEPALIVE int web_import_mod(const char *name, const char *text) { return mod_import_text(name, text); }
#endif

static void mods_scan(void)
{
    for (int i = 0; i < nmods; i++) free(mods[i].text);
    nmods = 0;
#ifdef PLATFORM_WEB
    snprintf(mods_path, sizeof mods_path, "browser storage");
    char *l = web_mods_list();
    for (char *p = l; p && *p;) {
        char *a = strchr(p, 1);
        if (!a) break;
        char *b = strchr(a, 2);
        if (!b) break;
        *a = 0; *b = 0;
        mod_add(p, a + 1);
        p = b + 1;
    }
    free(l);
    if (nmods == 0) { web_mods_put("example_mod.txt", EXAMPLE_MOD); mod_add("example_mod.txt", EXAMPLE_MOD); }
#else
    snprintf(mods_path, sizeof mods_path, "%s/mods", save_dir());
    MakeDirectory(mods_path);
    char ex[400];
    snprintf(ex, sizeof ex, "%s/example_mod.txt", mods_path);
    if (!FileExists(ex)) write_text(ex, EXAMPLE_MOD);
    const char *dirs[2] = { mods_path, NULL };
#if defined(PLATFORM_DESKTOP)
    static char appmods[400];
    snprintf(appmods, sizeof appmods, "%smods", GetApplicationDirectory());
    if (DirectoryExists(appmods) && strcmp(appmods, mods_path)) dirs[1] = appmods;
#endif
    for (int d = 0; d < 2; d++) {
        if (!dirs[d] || !DirectoryExists(dirs[d])) continue;
        FilePathList fl = LoadDirectoryFilesEx(dirs[d], ".txt", false);
        for (unsigned i = 0; i < fl.count; i++) {
            char *t = LoadFileText(fl.paths[i]);
            if (!t) continue;
            mod_add(GetFileName(fl.paths[i]), t);
            UnloadFileText(t);
        }
        UnloadDirectoryFiles(fl);
    }
#endif
}

void data_load(void)
{
    nitems = 0; nrecipes = 0; nstart = 0;
    memset(empties, 0, sizeof empties);
    memset(extra_rest, 0, sizeof extra_rest); memset(extra_heal, 0, sizeof extra_heal); memset(extra_warm, 0, sizeof extra_warm);
    tune_hunger = tune_thirst = tune_fuel = tune_raiders = tune_loot = tune_cold = 1;
    data_errors[0] = 0;
    load_text(BASE, "base", false, NULL, 0);
    mods_scan();
    for (int i = 0; i < nmods; i++)
        if (mods[i].on) { char nm[48]; load_text(mods[i].text, mods[i].file, true, nm, sizeof nm); }
    if (data_errors[0]) TraceLog(LOG_WARNING, "data: %s", data_errors);
}
