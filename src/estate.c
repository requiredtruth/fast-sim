#include "estate.h"
#include "ped.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

bool ambience_enabled = true, vehicle_audio_enabled = true, female_avatar;
Mesh estate_cube, garden_tool_mesh[4];

enum { FLOOR, WALL, FRAME, DOOR, ROOF, SOIL, PIECE_COUNT };
static const char *const piece_ids[] = { "floor_panel", "wall_panel", "door_frame", "wood_door", "roof_panel", "soil_plot" };
Garment garments[] = {
#define GARMENT(id,name,color,group,style) {#id,0x##color,GARMENT_##group,style,{0},{0}},
#define CROP(seed,id,name,color,style,days) 
#define CAR_PART(id,name,effect,shape) 
#include "catalog.def"
#undef GARMENT
#undef CROP
#undef CAR_PART
};
Crop crops[] = {
#define GARMENT(id,name,color,group,style) 
#define CROP(seed,id,name,color,style,days) {#seed,#id,name,0x##color,days,{0}},
#define CAR_PART(id,name,effect,shape) 
#include "catalog.def"
#undef GARMENT
#undef CROP
#undef CAR_PART
};
CarPart repair_parts[] = {
#define GARMENT(id,name,color,group,style) 
#define CROP(seed,id,name,color,style,days) 
#define CAR_PART(id,name,effect,shape) {#id,shape,{0}},
#include "catalog.def"
#undef GARMENT
#undef CROP
#undef CAR_PART
};
const int garment_count=sizeof garments/sizeof *garments;
const int crop_count=sizeof crops/sizeof *crops;
const int repair_part_count=sizeof repair_parts/sizeof *repair_parts;

static Color color_from_rgb(unsigned color) { return (Color){color>>16,color>>8,color,255}; }

static int garment_index(int item)
{
    if(item<0 || item>=nitems)return -1;
    for(int i=0;i<garment_count;i++)if(!strcmp(garments[i].id,items[item].id))return i;
    return -1;
}

Mesh catalog_item_mesh(int item)
{
    if(item<0 || item>=nitems)return (Mesh){0};
    const char *id=items[item].id;
    static const char *const tools[]={"garden_hoe","watering_can","compost","hand_trowel"};
    for(int i=0;i<4;i++)if(!strcmp(id,tools[i]))return garden_tool_mesh[i];
    int g=garment_index(item);
    if(g>=0)return garments[g].placed;
    for(int i=0;i<crop_count;i++)if(!strcmp(id,crops[i].id))return crops[i].mesh;
    for(int i=0;i<repair_part_count;i++)if(!strcmp(id,repair_parts[i].id))return repair_parts[i].mesh;
    return item_mesh[items[item].icon];
}

static int seed_index(int item)
{
    if(item<0 || item>=nitems)return -1;
    for(int i=0;i<crop_count;i++)if(!strcmp(crops[i].seed,items[item].id))return i;
    return -1;
}

static bool till_tool(int item)
{
    return item>=0 && (item==item_find("garden_hoe") || item==item_find("hand_trowel"));
}

bool garden_item(int item)
{
    return till_tool(item) || seed_index(item)>=0 || (item>=0 && (item==item_find("compost") || item==item_find("watering_can")));
}

static int outfit[2] = {-1,-1};

typedef struct { double x,z; float y,yaw; double planted; int kind, finish, crop; bool open, seeded, wet; } Piece;
#define ESTATE_LIMIT 256
static Piece pieces[ESTATE_LIMIT];
static int count;

static int lookup(int item, const char *const *ids, int n)
{
    if (item < 0 || item >= nitems) return -1;
    for (int i=0; i<n; i++) if (!strcmp(items[item].id, ids[i])) return i;
    return -1;
}

bool estate_piece(int item) { return lookup(item,piece_ids,PIECE_COUNT)>=0 || till_tool(item); }
bool wardrobe_item(int item) { return garment_index(item)>=0; }
void estate_reset(void) { count=0; outfit[0]=outfit[1]=-1; }
static Vector3 local(const Piece *p) { return (Vector3){(float)(p->x-world_ox),p->y,(float)(p->z-world_oz)}; }

bool estate_place(int item, Vector3 at, float yaw)
{
    int kind = lookup(item,piece_ids,PIECE_COUNT);
    bool tilled=till_tool(item);
    if(tilled){
        if(on_road(at.x,at.z) || fabsf(ground_y(at.x,at.z)-at.y)>.20f){hud_msg("Till dry ground away from roads and floors",2);return false;}
        kind=SOIL;
    }
    if (kind<0 || count==ESTATE_LIMIT) return false;
    yaw = roundf(yaw/(PI/2))*(PI/2);
    bool framed=false;
    at.y -= .025f;
    for (int i=0;i<count;i++) {
        Piece *p=&pieces[i]; Vector3 b=local(p);
        float separation=kind==SOIL && p->kind==SOIL?1.8f:.48f;
        if (hypotf(at.x-b.x,at.z-b.z)>separation || fabsf(at.y-b.y)>.35f) continue;
        if (kind==DOOR && p->kind==FRAME) { at=b; yaw=p->yaw; framed=true; continue; }
        if (!(kind==WALL || kind==FRAME) || p->kind!=FLOOR) return false;
    }
    if (kind==DOOR && !framed) { hud_msg("Aim at the base of a door frame",2); return false; }
    pieces[count++]=(Piece){.x=at.x+world_ox,.z=at.z+world_oz,.y=at.y,.yaw=yaw,.kind=kind,.finish=tilled?1:0};
    return true;
}

static WBox part_box(const Piece *p, Vector3 at, float x, float hx, float hz, float height)
{
    Vector3 ax={cosf(p->yaw),0,-sinf(p->yaw)}, az={sinf(p->yaw),0,cosf(p->yaw)};
    Vector3 center=Vector3Add(at,Vector3Scale(ax,x));
    return (WBox){center,hx,hz,at.y,at.y+height,ax,az};
}

int estate_boxes(Vector3 at,float radius,WBox *out,int max)
{
    int n=0;
    for(int i=0;i<count && n<max;i++){
        const Piece *p=&pieces[i];
        if(p->kind!=WALL && p->kind!=DOOR && p->kind!=FRAME) continue;
        if(p->kind==DOOR && p->open) continue;
        Vector3 center=local(p);
        if(hypotf(center.x-at.x,center.z-at.z)>radius+1.1f) continue;
        if(p->kind==FRAME){
            out[n++]=part_box(p,center,-.67f,.08f,.11f,2.24f);
            if(n<max) out[n++]=part_box(p,center,.67f,.08f,.11f,2.24f);
        } else out[n++]=part_box(p,center,0,p->kind==WALL?1:.56f,.09f,p->kind==WALL?2.16f:2.02f);
    }
    return n;
}

bool estate_push(Vector3 *at,float radius,float height)
{
    WBox boxes[24]; int n=estate_boxes(*at,radius,boxes,24);
    bool hit=false;
    for(int i=0;i<n;i++){
        WBox *b=&boxes[i];
        if(at->y+.25f>b->y1 || at->y+height<b->y0) continue;
        Vector3 delta=Vector3Subtract(*at,b->c);
        float x=Vector3DotProduct(delta,b->ax), z=Vector3DotProduct(delta,b->az);
        float dx=b->hx+radius-fabsf(x), dz=b->hz+radius-fabsf(z);
        if(dx<=0 || dz<=0) continue;
        Vector3 axis=dx<dz?b->ax:b->az;
        float shift=dx<dz?copysignf(dx,x):copysignf(dz,z);
        *at=Vector3Add(*at,Vector3Scale(axis,shift));
        hit=true;
    }
    return hit;
}

static int aimed(bool reclaim)
{
    Ped *p=&peds[PLAYER];
    if (p->car>=0) return -1;
    Vector3 forward=Vector3Normalize(Vector3Subtract(cam.target,cam.position));
    Hit obstruction;
    float best=world_ray(cam.position,forward,15,&obstruction)?obstruction.t+.16f:15;
    int index=-1;
    for(int i=0;i<count;i++){
        Piece *s=&pieces[i]; Vector3 a=local(s);
        if(hypotf(p->pos.x-a.x,p->pos.z-a.z)>3.6f) continue;
        float y=s->kind==ROOF?2.5f:s->kind==SOIL || s->kind==FLOOR?.18f:2.3f;
        float r=s->kind==SOIL || s->kind==FLOOR || s->kind==ROOF?1.02f:.85f;
        BoundingBox box={{a.x-r,a.y+(s->kind==ROOF?2.3f:-.08f),a.z-r},{a.x+r,a.y+y,a.z+r}};
        RayCollision hit=GetRayCollisionBox((Ray){cam.position,forward},box);
        if(!hit.hit || hit.distance>best+.01f) continue;
        if(index>=0 && fabsf(hit.distance-best)<.01f && pieces[index].kind==DOOR && s->kind!=DOOR) continue;
        if(!reclaim && s->kind!=DOOR && s->kind!=SOIL) continue;
        best=hit.distance; index=i;
    }
    return index;
}

bool estate_targeted(void) { return aimed(true)>=0; }

bool estate_paint(void)
{
    int i=aimed(true);
    if(i<0)return false;
    Piece *p=&pieces[i];
    if(p->kind==SOIL)return false;
    p->finish=(p->finish+1)%4;
    hud_msg("Finish changed",2);
    return true;
}

static bool ready(const Piece *p){ return p->wet && sv.days-p->planted >= crops[p->crop].days; }

static bool plant(Piece *p,int crop)
{
    if(crop<0 || p->seeded || !pack_take(pack,PACK_SLOTS,item_find(crops[crop].seed),1))return false;
    p->crop=crop;
    p->seeded=true;
    hud_msg("Seeds planted",2);
    return true;
}

bool garden_use(int item)
{
    if(!garden_item(item))return false;
    if(till_tool(item)){
        for(int i=0;i<PACK_SLOTS;i++)if(pack[i].n && pack[i].item==item){ placement_begin(i);return true; }
        return false;
    }
    int i=aimed(false);
    if(i<0 || pieces[i].kind!=SOIL){hud_msg("Aim at a garden plot first",2);return false;}
    Piece *p=&pieces[i];
    int crop=seed_index(item);
    if(crop>=0){
        if(!plant(p,crop)){hud_msg("This plot already has a crop",2);return false;}
        return true;
    }
    if(item==item_find("watering_can")){
        if(!p->seeded || p->wet){hud_msg("This plot does not need water",2);return false;}
        return estate_use(false);
    }
    if(!p->seeded || !p->wet){hud_msg("Water the crop before adding compost",2);return false;}
    if(!pack_take(pack,PACK_SLOTS,item,1))return false;
    p->planted-=.025;
    hud_msg("Compost added",2);
    return true;
}

const char *estate_prompt(void)
{
    int i=aimed(false);
    if(i<0) return NULL;
    Piece *p=&pieces[i];
    if(p->kind==DOOR) return p->open?"Close door":"Open door";
    if(!p->seeded)return "Plant seeds";
    static char label[64];
    snprintf(label,sizeof label,"%s %s",!p->wet?"Water":ready(p)?"Harvest":"Growing",crops[p->crop].name);
    return label;
}

bool estate_use(bool reclaim)
{
    int i=aimed(reclaim);
    if(i<0) return false;
    Piece *p=&pieces[i];
    if(reclaim){
        if(p->kind==FRAME) for(int j=0;j<count;j++)
            if(pieces[j].kind==DOOR && hypot(pieces[j].x-p->x,pieces[j].z-p->z)<.12 && fabsf(pieces[j].y-p->y)<.12f) {
                hud_msg("Remove the door first",2);return true;
            }
        if(p->kind==SOIL && p->finish==1) { pieces[i]=pieces[--count]; hud_msg("Ground restored",2); }
        else if(pack_add(pack,PACK_SLOTS,item_find(piece_ids[p->kind]),1)) hud_msg("Pack full",2);
        else { pieces[i]=pieces[--count]; hud_msg("Part recovered",2); }
    } else if(p->kind==DOOR) p->open=!p->open;
    else if(!p->seeded){
        for(int crop=0;crop<crop_count;crop++)if(plant(p,crop))return true;
        hud_msg("A seed packet is needed",2);
    } else if(!p->wet){
        if(pack_take(pack,PACK_SLOTS,item_find("water"),1)){
            pack_add(pack,PACK_SLOTS,item_find("bottle"),1);p->wet=true;p->planted=sv.days;hud_msg("Crop watered",2);
        } else hud_msg("A bottle of water is needed",2);
    } else if(ready(p)){
        int crop=item_find(crops[p->crop].id);
        if(pack_add(pack,PACK_SLOTS,crop,1)) hud_msg("Pack full",2);
        else { p->wet=false;hud_msg("Crop harvested. Water for another crop",2); }
    } else hud_msg("Crop is still growing",2);
    return true;
}

static void block(Vector3 p,float yaw,Vector3 offset,Vector3 size,Color tint)
{
    Matrix rotation=MatrixRotateY(yaw);
    Vector3 at=Vector3Add(p,Vector3Transform(offset,rotation));
    Matrix transform=MatrixMultiply(MatrixMultiply(MatrixScale(size.x,size.y,size.z),rotation),MatrixTranslate(at.x,at.y,at.z));
    draw_mesh(estate_cube,transform,tint,MAT_PROP);
}

void estate_draw(int pass)
{
    (void)pass;
    static const Color finishes[]={{143,98,56,255},{62,112,146,255},{152,75,61,255},{185,179,153,255}};
    const Color edge={95,59,33,255},leaf={72,136,50,255};
    for(int i=0;i<count;i++){
        Piece *s=&pieces[i]; Vector3 p=local(s);
        if(Vector3DistanceSqr(p,cam.position)>150*150)continue;
        float a=s->yaw;
        Color wood=finishes[s->finish];
        if(s->kind==FRAME){
            block(p,a,(Vector3){-.67f,1.12f,0},(Vector3){.16f,2.24f,.22f},edge);
            block(p,a,(Vector3){ .67f,1.12f,0},(Vector3){.16f,2.24f,.22f},edge);
            block(p,a,(Vector3){0,2.17f,0},(Vector3){1.5f,.15f,.22f},wood);
        } else if(s->kind==DOOR){
            Vector3 hinge=Vector3Add(p,Vector3Transform((Vector3){-.57f,0,0},MatrixRotateY(a)));
            if(s->open) a+=PI*.5f;
            block(hinge,a,(Vector3){.56f,1.01f,0},(Vector3){1.12f,2.02f,.10f},wood);
            block(hinge,a,(Vector3){.96f,1.05f,.08f},(Vector3){.10f,.07f,.10f},edge);
        } else if(s->kind==WALL){
            for(int j=0;j<6;j++)block(p,a,(Vector3){0,.18f+j*.36f,0},(Vector3){2,.355f,.17f},j%2?edge:wood);
        } else if(s->kind==SOIL){
            block(p,a,(Vector3){0,.04f,0},(Vector3){1.8f,.13f,1.4f},(Color){90,60,40,255});
            if(s->seeded) for(int j=-1;j<=1;j++){
                float h=ready(s)?.48f:s->wet?.26f:.13f;
                block(p,a,(Vector3){j*.42f,h*.5f+.09f,0},(Vector3){.08f,h,.08f},leaf);
                block(p,a+.65f,(Vector3){j*.42f,h+.05f,0},(Vector3){.19f,.09f,.12f},leaf);
                if(ready(s)){
                    Vector3 fruit=Vector3Add(p,Vector3Transform((Vector3){j*.42f,h+.03f,0},MatrixRotateY(a)));
                    draw_mesh(crops[s->crop].mesh,MatrixMultiply(MatrixScale(.75f,.75f,.75f),
                              MatrixTranslate(fruit.x,fruit.y,fruit.z)),color_from_rgb(crops[s->crop].color),MAT_PROP);
                }
            }
        } else {
            float y=s->kind==ROOF?2.23f:.06f;
            for(int j=0;j<5;j++)block(p,a,(Vector3){(j-2)*.4f,y,0},(Vector3){.39f,.12f,2},j%2?edge:wood);
        }
    }
}

void wardrobe_apply(void)
{
    Ped *p=&peds[PLAYER];
    if(!p->used)return;
    for(int slot=0;slot<2;slot++)
        if(outfit[slot]>=0 && !pack_count(item_find(garments[outfit[slot]].id)))outfit[slot]=-1;
    p->body=female_avatar?BODY_FEMALE:BODY_MALE;
    p->hair=female_avatar?HAIR_LONG:HAIR_BUZZ;
    p->pal[R_SHIRT]=outfit[0]>=0?color_from_rgb(garments[outfit[0]].color):(Color){176,120,64,255};
}

bool wardrobe_equip(int item)
{
    int k=garment_index(item);
    if(k<0 || !pack_count(item))return false;
    int slot=garments[k].group==GARMENT_HAT;
    outfit[slot]=outfit[slot]==k?-1:k;
    wardrobe_apply();
    hud_msg(outfit[slot]<0?"Unequipped":"Equipped",2);
    return true;
}

void wardrobe_draw(Matrix model, Matrix *bones)
{
    if(outfit[0]>=0)draw_char(garments[outfit[0]].worn,model,bones,peds[PLAYER].pal);
    if(outfit[1]>=0){
        Color pal[R_COUNT]; memcpy(pal,peds[PLAYER].pal,sizeof pal);
        pal[R_HAIR]=color_from_rgb(garments[outfit[1]].color);
        draw_char(garments[outfit[1]].worn,model,bones,pal);
    }
}

bool estate_save(char **out,char *end)
{
    for(int i=-1;i<count;i++){
        Piece *p=i<0?NULL:&pieces[i];
        int n=p?snprintf(*out,end-*out,"estate=%d %.6f %.6f %.4f %.4f %d %d %d %.6f %d %d\n",p->kind,p->x,p->z,p->y,p->yaw,p->open,p->seeded,p->wet,p->planted,p->finish,p->crop):
                snprintf(*out,end-*out,"wardrobe=%s %s\n",outfit[0]>=0?garments[outfit[0]].id:"-",outfit[1]>=0?garments[outfit[1]].id:"-");
        if(n<0 || n>=end-*out)return false;
        *out+=n;
    }
    return true;
}

void estate_load(const char *save)
{
    const char *q=strstr(save,"\nwardrobe=");
    char top[24],hat[24];
    if(q && sscanf(q+1,"wardrobe=%23s %23s",top,hat)==2){
        outfit[0]=garment_index(item_find(top));
        outfit[1]=garment_index(item_find(hat));
        if(outfit[0]>=0 && garments[outfit[0]].group==GARMENT_HAT)outfit[0]=-1;
        if(outfit[1]>=0 && garments[outfit[1]].group!=GARMENT_HAT)outfit[1]=-1;
    }
    wardrobe_apply();
    q=save;
    while(count<ESTATE_LIMIT && (q=strstr(q,"\nestate="))){
        Piece p={0};int open,seeded,wet;
        if(sscanf(++q,"estate=%d %lf %lf %f %f %d %d %d %lf %d %d",&p.kind,&p.x,&p.z,&p.y,&p.yaw,&open,&seeded,&wet,&p.planted,&p.finish,&p.crop)<9 ||
           p.kind<0 || p.kind>=PIECE_COUNT || !isfinite(p.x) || !isfinite(p.z) || !isfinite(p.y) || !isfinite(p.yaw) || !isfinite(p.planted))continue;
        p.open=open;p.seeded=seeded;p.wet=wet;
        if(p.finish<0 || p.finish>3)p.finish=0;
        if(p.crop<0 || p.crop>=crop_count)p.crop=0;
        pieces[count++]=p;
    }
}
