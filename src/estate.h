#ifndef ESTATE_H
#define ESTATE_H
#include "city.h"

extern bool ambience_enabled, vehicle_audio_enabled, female_avatar;
extern Mesh estate_cube, wardrobe_mesh[3];
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
