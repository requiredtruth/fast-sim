#ifndef ESTATE_H
#define ESTATE_H
#include "city.h"

extern bool ambience_enabled, vehicle_audio_enabled, female_avatar;
extern Mesh estate_cube, garden_tool_mesh[4];
enum { GARMENT_JACKET, GARMENT_DRESS, GARMENT_HAT };
typedef struct { const char *id; unsigned color; int group, style; Mesh worn, placed; } Garment;
typedef struct { const char *seed, *id, *name; unsigned color; float days; Mesh mesh; } Crop;
typedef struct { const char *id; int shape; Mesh mesh; } CarPart;
extern Garment garments[];
extern Crop crops[];
extern CarPart repair_parts[];
extern const int garment_count, crop_count, repair_part_count;
Mesh catalog_item_mesh(int item);
bool garden_item(int item);
bool garden_use(int item);
void estate_reset(void);
bool estate_place(int item, Vector3 p, float yaw);
bool estate_piece(int item);
const char *estate_prompt(void);
bool estate_use(bool reclaim);
bool estate_targeted(void);
bool estate_paint(void);
void estate_draw(int pass);
int estate_boxes(Vector3 p, float radius, WBox *out, int max);
bool estate_push(Vector3 *p, float radius, float height);
bool estate_save(char **out, char *end);
void estate_load(const char *save);
bool wardrobe_item(int item);
bool wardrobe_equip(int item);
void wardrobe_apply(void);
void wardrobe_draw(Matrix model, Matrix *bones);
#endif
