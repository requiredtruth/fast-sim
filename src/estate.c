#include "estate.h"
#include "ped.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

bool ambience_enabled = true, vehicle_audio_enabled = true, female_avatar;
Mesh estate_cube, wardrobe_mesh[3];

enum { FLOOR, WALL, FRAME, DOOR, ROOF, SOIL, PIECE_COUNT };
static const char *const piece_ids[] = { "floor_panel", "wall_panel", "door_frame", "wood_door", "roof_panel", "soil_plot" };
static const char *const garment_ids[] = { "leather_jacket", "rose_dress", "blue_dress", "cowboy_hat" };
static const Color garment_color[] = {{86,49,34,255},{218,67,132,255},{65,132,205,255},{147,101,54,255}};
static int outfit[2] = {-1,-1};

typedef struct { double x,z; float y,yaw; double planted; int kind, finish; bool open, seeded, wet; } Piece;
#define ESTATE_LIMIT 256
static Piece pieces[ESTATE_LIMIT];
static int count;

static int lookup(int item, const char *const *ids, int n)
{
    if (item < 0 || item >= nitems) return -1;
    for (int i=0; i<n; i++) if (!strcmp(items[item].id, ids[i])) return i;
    return -1;
}

bool estate_piece(int item) { return lookup(item,piece_ids,PIECE_COUNT)>=0; }
bool wardrobe_item(int item) { return lookup(item,garment_ids,4)>=0; }
void estate_reset(void) { count=0; outfit[0]=outfit[1]=-1; }
static Vector3 local(const Piece *p) { return (Vector3){(float)(p->x-world_ox),p->y,(float)(p->z-world_oz)}; }

bool estate_place(int item, Vector3 at, float yaw)
{
    int kind = lookup(item,piece_ids,PIECE_COUNT);
    if (kind<0 || count==ESTATE_LIMIT) return false;
    yaw = roundf(yaw/(PI/2))*(PI/2);
    bool framed=false;
    at.y -= .025f;
    for (int i=0;i<count;i++) {
        Piece *p=&pieces[i]; Vector3 b=local(p);
        if (hypotf(at.x-b.x,at.z-b.z)>.48f || fabsf(at.y-b.y)>.35f) continue;
        if (kind==DOOR && p->kind==FRAME) { at=b; yaw=p->yaw; framed=true; continue; }
        if (!(kind==WALL || kind==FRAME) || p->kind!=FLOOR) return false;
    }
    if (kind==DOOR && !framed) { hud_msg("Aim at the base of a door frame",2); return false; }
    pieces[count++]=(Piece){.x=at.x+world_ox,.z=at.z+world_oz,.y=at.y,.yaw=yaw,.kind=kind};
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

static bool ready(const Piece *p){ return p->wet && sv.days-p->planted >= .075; }

const char *estate_prompt(void)
{
    int i=aimed(false);
    if(i<0) return NULL;
    Piece *p=&pieces[i];
    if(p->kind==DOOR) return p->open?"Close door":"Open door";
    return !p->seeded?"Plant bean seed":!p->wet?"Water beans":ready(p)?"Harvest beans":"Beans are growing";
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
        if(pack_add(pack,PACK_SLOTS,item_find(piece_ids[p->kind]),1)) hud_msg("Pack full",2);
        else { pieces[i]=pieces[--count]; hud_msg("Part recovered",2); }
    } else if(p->kind==DOOR) p->open=!p->open;
    else if(!p->seeded){
        if(pack_take(pack,PACK_SLOTS,item_find("bean_seed"),1)){p->seeded=true;hud_msg("Beans planted",2);}
        else hud_msg("A bean seed is needed",2);
    } else if(!p->wet){
        if(pack_take(pack,PACK_SLOTS,item_find("water"),1)){
            pack_add(pack,PACK_SLOTS,item_find("bottle"),1);p->wet=true;p->planted=sv.days;hud_msg("Beans watered",2);
        } else hud_msg("A bottle of water is needed",2);
    } else if(ready(p)){
        int crop=item_find("garden_beans");
        if(pack_add(pack,PACK_SLOTS,crop,1)) hud_msg("Pack full",2);
        else { p->wet=false;hud_msg("Beans harvested. Water for another crop",2); }
    } else hud_msg("Beans are still growing",2);
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
        if(outfit[slot]>=0 && !pack_count(item_find(garment_ids[outfit[slot]])))outfit[slot]=-1;
    p->body=female_avatar?BODY_FEMALE:BODY_MALE;
    p->hair=female_avatar?HAIR_LONG:HAIR_BUZZ;
    p->pal[R_SHIRT]=outfit[0]>=0?garment_color[outfit[0]]:(Color){176,120,64,255};
}

bool wardrobe_equip(int item)
{
    int k=lookup(item,garment_ids,4);
    if(k<0 || !pack_count(item))return false;
    int slot=k==3;
    outfit[slot]=outfit[slot]==k?-1:k;
    wardrobe_apply();
    hud_msg(outfit[slot]<0?"Unequipped":"Equipped",2);
    return true;
}

void wardrobe_draw(Matrix model, Matrix *bones)
{
    if(outfit[0]>=0)draw_char(wardrobe_mesh[outfit[0]==0?0:1],model,bones,peds[PLAYER].pal);
    if(outfit[1]>=0){
        Color pal[R_COUNT]; memcpy(pal,peds[PLAYER].pal,sizeof pal);
        pal[R_HAIR]=garment_color[3];
        draw_char(wardrobe_mesh[2],model,bones,pal);
    }
}

bool estate_save(char **out,char *end)
{
    for(int i=-1;i<count;i++){
        Piece *p=i<0?NULL:&pieces[i];
        int n=p?snprintf(*out,end-*out,"estate=%d %.6f %.6f %.4f %.4f %d %d %d %.6f %d\n",p->kind,p->x,p->z,p->y,p->yaw,p->open,p->seeded,p->wet,p->planted,p->finish):
                snprintf(*out,end-*out,"outfit=%d %d\n",outfit[0],outfit[1]);
        if(n<0 || n>=end-*out)return false;
        *out+=n;
    }
    return true;
}

void estate_load(const char *save)
{
    const char *q=strstr(save,"\noutfit=");
    int top=-1,hat=-1;
    if(q && sscanf(q+1,"outfit=%d %d",&top,&hat)==2){
        outfit[0]=top>=0 && top<3 && pack_count(item_find(garment_ids[top]))?top:-1;
        outfit[1]=hat==3 && pack_count(item_find(garment_ids[3]))?3:-1;
    }
    wardrobe_apply();
    q=save;
    while(count<ESTATE_LIMIT && (q=strstr(q,"\nestate="))){
        Piece p={0};int open,seeded,wet;
        if(sscanf(++q,"estate=%d %lf %lf %f %f %d %d %d %lf %d",&p.kind,&p.x,&p.z,&p.y,&p.yaw,&open,&seeded,&wet,&p.planted,&p.finish)<9 ||
           p.kind<0 || p.kind>=PIECE_COUNT || !isfinite(p.x) || !isfinite(p.z) || !isfinite(p.y) || !isfinite(p.yaw) || !isfinite(p.planted))continue;
        p.open=open;p.seeded=seeded;p.wet=wet;
        if(p.finish<0 || p.finish>3)p.finish=0;
        pieces[count++]=p;
    }
}
