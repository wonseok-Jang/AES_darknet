#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "image.h"
#include "utils.h"
#include "blas.h"
#include "dark_cuda.h"
#include <stdio.h>

#include <unistd.h>
#include <net/if.h>
#include <sys/types.h>
#include <sys/ioctl.h>
#include <sys/socket.h>
#include <linux/can.h>
#include <linux/can/raw.h>

#ifndef _USE_MATH_DEFINES
#define _USE_MATH_DEFINES
#endif
#include <math.h>

#ifndef STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"
#endif
#ifndef STB_IMAGE_WRITE_IMPLEMENTATION
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb_image_write.h"
#endif

#define ARRAY_SIZE 20
#define __MAX(a,b) (((a)>(b))?(a):(b))
#define __MIN(a,b) (((a)<(b))?(a):(b))

struct can_frame frames[2][ARRAY_SIZE];
struct object_info{
	int left;
	int right;
	int top;
	int bot;
	int class;
	int id;
}object_info[2][ARRAY_SIZE];

char id_array[2][ARRAY_SIZE]={0};
int prior_object_number=0;
int frame_index=0;
int For_Sync=1;
//int cnt=0;
//double standard_time=0;
int sw;

//char *p;
int curr_timestamp;
int prev_timestamp;

extern int check_mistakes;
//int windows = 0;

float colors[6][3] = { {1,0,1}, {0,0,1},{0,1,1},{0,1,0},{1,1,0},{1,0,0} };

float get_color(int c, int x, int max)
{
    float ratio = ((float)x/max)*5;
    int i = floor(ratio);
    int j = ceil(ratio);
    ratio -= i;
    float r = (1-ratio) * colors[i][c] + ratio*colors[j][c];
    //printf("%f\n", r);
    return r;
}

static float get_pixel(image m, int x, int y, int c)
{
    assert(x < m.w && y < m.h && c < m.c);
    return m.data[c*m.h*m.w + y*m.w + x];
}
static float get_pixel_extend(image m, int x, int y, int c)
{
    if (x < 0 || x >= m.w || y < 0 || y >= m.h) return 0;
    /*
    if(x < 0) x = 0;
    if(x >= m.w) x = m.w-1;
    if(y < 0) y = 0;
    if(y >= m.h) y = m.h-1;
    */
    if (c < 0 || c >= m.c) return 0;
    return get_pixel(m, x, y, c);
}
static void set_pixel(image m, int x, int y, int c, float val)
{
    if (x < 0 || y < 0 || c < 0 || x >= m.w || y >= m.h || c >= m.c) return;
    assert(x < m.w && y < m.h && c < m.c);
    m.data[c*m.h*m.w + y*m.w + x] = val;
}
static void add_pixel(image m, int x, int y, int c, float val)
{
    assert(x < m.w && y < m.h && c < m.c);
    m.data[c*m.h*m.w + y*m.w + x] += val;
}

void composite_image(image source, image dest, int dx, int dy)
{
    int x,y,k;
    for(k = 0; k < source.c; ++k){
        for(y = 0; y < source.h; ++y){
            for(x = 0; x < source.w; ++x){
                float val = get_pixel(source, x, y, k);
                float val2 = get_pixel_extend(dest, dx+x, dy+y, k);
                set_pixel(dest, dx+x, dy+y, k, val * val2);
            }
        }
    }
}

image border_image(image a, int border)
{
    image b = make_image(a.w + 2*border, a.h + 2*border, a.c);
    int x,y,k;
    for(k = 0; k < b.c; ++k){
        for(y = 0; y < b.h; ++y){
            for(x = 0; x < b.w; ++x){
                float val = get_pixel_extend(a, x - border, y - border, k);
                if(x - border < 0 || x - border >= a.w || y - border < 0 || y - border >= a.h) val = 1;
                set_pixel(b, x, y, k, val);
            }
        }
    }
    return b;
}

image tile_images(image a, image b, int dx)
{
    if(a.w == 0) return copy_image(b);
    image c = make_image(a.w + b.w + dx, (a.h > b.h) ? a.h : b.h, (a.c > b.c) ? a.c : b.c);
    fill_cpu(c.w*c.h*c.c, 1, c.data, 1);
    embed_image(a, c, 0, 0);
    composite_image(b, c, a.w + dx, 0);
    return c;
}

image get_label(image **characters, char *string, int size)
{
    if(size > 7) size = 7;
    image label = make_empty_image(0,0,0);
    while(*string){
        image l = characters[size][(int)*string];
        image n = tile_images(label, l, -size - 1 + (size+1)/2);
        free_image(label);
        label = n;
        ++string;
    }
    image b = border_image(label, label.h*.25);
    free_image(label);
    return b;
}

image get_label_v3(image **characters, char *string, int size)
{
    size = size / 10;
    if (size > 7) size = 7;
    image label = make_empty_image(0, 0, 0);
    while (*string) {
        image l = characters[size][(int)*string];
        image n = tile_images(label, l, -size - 1 + (size + 1) / 2);
        free_image(label);
        label = n;
        ++string;
    }
    image b = border_image(label, label.h*.05);
    free_image(label);
    return b;
}

void draw_label(image a, int r, int c, image label, const float *rgb)
{
    int w = label.w;
    int h = label.h;
    if (r - h >= 0) r = r - h;

    int i, j, k;
    for(j = 0; j < h && j + r < a.h; ++j){
        for(i = 0; i < w && i + c < a.w; ++i){
            for(k = 0; k < label.c; ++k){
                float val = get_pixel(label, i, j, k);
                set_pixel(a, i+c, j+r, k, rgb[k] * val);
            }
        }
    }
}

void draw_weighted_label(image a, int r, int c, image label, const float *rgb, const float alpha)
{
    int w = label.w;
    int h = label.h;
    if (r - h >= 0) r = r - h;

    int i, j, k;
    for (j = 0; j < h && j + r < a.h; ++j) {
        for (i = 0; i < w && i + c < a.w; ++i) {
            for (k = 0; k < label.c; ++k) {
                float val1 = get_pixel(label, i, j, k);
                float val2 = get_pixel(a, i + c, j + r, k);
                float val_dst = val1 * rgb[k] * alpha + val2 * (1 - alpha);
                set_pixel(a, i + c, j + r, k, val_dst);
            }
        }
    }
}

void draw_box_bw(image a, int x1, int y1, int x2, int y2, float brightness)
{
    //normalize_image(a);
    int i;
    if (x1 < 0) x1 = 0;
    if (x1 >= a.w) x1 = a.w - 1;
    if (x2 < 0) x2 = 0;
    if (x2 >= a.w) x2 = a.w - 1;

    if (y1 < 0) y1 = 0;
    if (y1 >= a.h) y1 = a.h - 1;
    if (y2 < 0) y2 = 0;
    if (y2 >= a.h) y2 = a.h - 1;

    for (i = x1; i <= x2; ++i) {
        a.data[i + y1*a.w + 0 * a.w*a.h] = brightness;
        a.data[i + y2*a.w + 0 * a.w*a.h] = brightness;
    }
    for (i = y1; i <= y2; ++i) {
        a.data[x1 + i*a.w + 0 * a.w*a.h] = brightness;
        a.data[x2 + i*a.w + 0 * a.w*a.h] = brightness;
    }
}

void draw_box_width_bw(image a, int x1, int y1, int x2, int y2, int w, float brightness)
{
    int i;
    for (i = 0; i < w; ++i) {
        float alternate_color = (w % 2) ? (brightness) : (1.0 - brightness);
        draw_box_bw(a, x1 + i, y1 + i, x2 - i, y2 - i, alternate_color);
    }
}

void draw_box(image a, int x1, int y1, int x2, int y2, float r, float g, float b)
{
    //normalize_image(a);
    int i;
    if(x1 < 0) x1 = 0;
    if(x1 >= a.w) x1 = a.w-1;
    if(x2 < 0) x2 = 0;
    if(x2 >= a.w) x2 = a.w-1;

    if(y1 < 0) y1 = 0;
    if(y1 >= a.h) y1 = a.h-1;
    if(y2 < 0) y2 = 0;
    if(y2 >= a.h) y2 = a.h-1;

    for(i = x1; i <= x2; ++i){
        a.data[i + y1*a.w + 0*a.w*a.h] = r;
        a.data[i + y2*a.w + 0*a.w*a.h] = r;

        a.data[i + y1*a.w + 1*a.w*a.h] = g;
        a.data[i + y2*a.w + 1*a.w*a.h] = g;

        a.data[i + y1*a.w + 2*a.w*a.h] = b;
        a.data[i + y2*a.w + 2*a.w*a.h] = b;
    }
    for(i = y1; i <= y2; ++i){
        a.data[x1 + i*a.w + 0*a.w*a.h] = r;
        a.data[x2 + i*a.w + 0*a.w*a.h] = r;

        a.data[x1 + i*a.w + 1*a.w*a.h] = g;
        a.data[x2 + i*a.w + 1*a.w*a.h] = g;

        a.data[x1 + i*a.w + 2*a.w*a.h] = b;
        a.data[x2 + i*a.w + 2*a.w*a.h] = b;
    }
}

void draw_box_width(image a, int x1, int y1, int x2, int y2, int w, float r, float g, float b)
{
    int i;
    for(i = 0; i < w; ++i){
        draw_box(a, x1+i, y1+i, x2-i, y2-i, r, g, b);
    }
}

void draw_bbox(image a, box bbox, int w, float r, float g, float b)
{
    int left  = (bbox.x-bbox.w/2)*a.w;
    int right = (bbox.x+bbox.w/2)*a.w;
    int top   = (bbox.y-bbox.h/2)*a.h;
    int bot   = (bbox.y+bbox.h/2)*a.h;

    int i;
    for(i = 0; i < w; ++i){
        draw_box(a, left+i, top+i, right-i, bot-i, r, g, b);
    }
}

image **load_alphabet()
{
    int i, j;
    const int nsize = 8;
    image** alphabets = (image**)xcalloc(nsize, sizeof(image*));
    for(j = 0; j < nsize; ++j){
        alphabets[j] = (image*)xcalloc(128, sizeof(image));
        for(i = 32; i < 127; ++i){
            char buff[256];
            sprintf(buff, "data/labels/%d_%d.png", i, j);
            alphabets[j][i] = load_image_color(buff, 0, 0);
        }
    }
    return alphabets;
}



// Creates array of detections with prob > thresh and fills best_class for them
detection_with_class* get_actual_detections(detection *dets, int dets_num, float thresh, int* selected_detections_num, char **names)
{
    int selected_num = 0;
    detection_with_class* result_arr = (detection_with_class*)xcalloc(dets_num, sizeof(detection_with_class));
    int i;
    for (i = 0; i < dets_num; ++i) {
        int best_class = -1;
        float best_class_prob = thresh;
        int j;
        for (j = 0; j < dets[i].classes; ++j) {
            int show = strncmp(names[j], "dont_show", 9);
            if (dets[i].prob[j] > best_class_prob && show) {
                best_class = j;
                best_class_prob = dets[i].prob[j];
            }
        }
        if (best_class >= 0) {
            result_arr[selected_num].det = dets[i];
            result_arr[selected_num].best_class = best_class;
            ++selected_num;
        }
    }
    if (selected_detections_num)
        *selected_detections_num = selected_num;
    return result_arr;
}

// compare to sort detection** by bbox.x
int compare_by_lefts(const void *a_ptr, const void *b_ptr) {
    const detection_with_class* a = (detection_with_class*)a_ptr;
    const detection_with_class* b = (detection_with_class*)b_ptr;
    const float delta = (a->det.bbox.x - a->det.bbox.w/2) - (b->det.bbox.x - b->det.bbox.w/2);
    return delta < 0 ? -1 : delta > 0 ? 1 : 0;
}

// compare to sort detection** by best_class probability
int compare_by_probs(const void *a_ptr, const void *b_ptr) {
    const detection_with_class* a = (detection_with_class*)a_ptr;
    const detection_with_class* b = (detection_with_class*)b_ptr;
    float delta = a->det.prob[a->best_class] - b->det.prob[b->best_class];
    return delta < 0 ? -1 : delta > 0 ? 1 : 0;
}

void SWAP(sorting* arr, int a, int b){
    sorting temp;
    temp=arr[a];
    arr[a]=arr[b];
    arr[b]=temp;
}

void sortCenX(sorting* arr, int m, int n){
    if(m<n){
        int key = m;
        int i = m + 1;
        int j = n;
        while(i <= j){
            while(i<=n && arr[i].tmp_cenX <= arr[key].tmp_cenX){
                i++;
            }
            while(j>m && arr[j].tmp_cenX >= arr[key].tmp_cenX){
                j--;
            }
            if(i > j){
                SWAP(arr, j, key);
            }
            else{
                SWAP(arr, i, j);
            }
        }
        sortCenX(arr, m, j-1);
        sortCenX(arr, j+1, n);
    }
}

int compute_iou(char *gt, int pred_xmin, int pred_ybot, int pred_xmax, int pred_ytop, float check){

    bool ok_cnt = false;
    int img_width = 1280;
    int img_height = 960;

	float gt_a = atof(*gt);
	float gt_b = atof(*(gt + 1));
	float gt_c = atof(*(gt + 2));
	float gt_d = atof(*(gt + 3));

//	printf("gt_a: %f\n", gt_a);
//	printf("gt_a check: %f\n", check);

    int gt_bbox_w = (int)(gt_c * img_width);
    int gt_bbox_h = (int)(gt_d * img_height);
    int gt_xmin = (int)((gt_a*img_width) - (gt_bbox_w/2));
    int gt_ybot = (int)((gt_b * img_height) + gt_bbox_h/2);
    int gt_xmax = (int)(gt_xmin + gt_bbox_w);
    int gt_ytop = (int)(gt_ybot - gt_bbox_h);

    int inter_area;
    float iou = 0.0;
    int x_a = __MAX(gt_xmin, pred_xmin);
    int y_a = __MIN(gt_ybot, pred_ybot);
    int x_b = __MIN(gt_xmax, pred_xmax);
    int y_b = __MAX(gt_ytop, pred_ytop);

    if (((x_b - x_a) > 0) && (y_a - y_b) > 0){
        inter_area = abs((x_b - x_a)) * abs((y_a - y_b));
        int gt_area = (gt_xmax - gt_xmin) * (gt_ybot -gt_ytop);
        int pred_area = (pred_xmax - pred_xmin) * (pred_ybot - pred_ytop);
        iou = (float) inter_area / (gt_area + pred_area - inter_area);
    }
    if (iou >= 0.5){
        ok_cnt = true;
    }
    return ok_cnt;
}

int draw_detections_with_tracking(image im, char *gt_input, detection *dets, int num, float thresh, char **names, image **alphabet, int classes, car_cnt *cnts, char *txt_path, int frame_count)
{
    bool txt_flag=false;
    FILE* fw1;
	int i,j;
    int box_cnt=0;

    if(txt_path!='\0'){
        fw1=fopen(txt_path,"a");// txt make;
        txt_flag=true;
    }

    sorting s[20];

    // For Carla mAP test -----------------------------
    int keep = 0;
    char *keep_ary[10] = {NULL,};
    char *p_gt_input = strtok(gt_input, "/");

    while(p_gt_input!=NULL){
        keep_ary[keep] = p_gt_input;
        keep++;
        p_gt_input = strtok(NULL, "/");
    }

    char dest[100] = "/mnt/hdd2/darknet/sanjabu/carla_result_text/";
    /////////strcat(dest, keep_ary[keep-1]);
    //printf("%s\n", dest);
    FILE *fp2 = fopen(dest, "a"); // For Carla mAP test
    // For Carla mAP test -----------------------------*

	// Read GT files.
	bool gt_flag = true;
	FILE *fp;
	if ((fp = fopen(gt_input, "r")) == NULL){
		gt_flag = false;
	}
	char pred_box[3][5][20];
	int label_cnt = 0;

	if(gt_flag == true){
		char text[256];
		char fuc[3][50];
		for(int i = 0; i < 3; i++){
			if(fgets(text, sizeof(text), fp) == NULL) break;
			else{
				label_cnt += 1;
				strcpy(fuc[i],text);
			}
		}
		for(int i = 0; i < label_cnt; i++){
			char *input_ptr = strtok(fuc[i], " ");
			int k = 0;
			while (input_ptr != NULL)
			{
				strcpy(pred_box[i][k], input_ptr);
				k = k + 1;
				input_ptr = strtok(NULL, " ");
			}
		}

		fclose(fp);
	}

	char str_tmp[1024];
	FILE *Image_time_File=fopen("FrontLeft.txt","r");

	//CAN COMMUNICATION variables
	int FVR=0;
	int FVI=0;
	int FVL=0;
	//int sr;
	struct sockaddr_can addr;

	struct can_frame frame;
	struct ifreq ifr;
	const char *ifname="can0";
	frame.can_dlc=8;
	//frame.can_id=0x000;
	for(int k = 0; k < frame.can_dlc; k++)
		frame.data[k] = 0x00;
	for(int k = 0; k < ARRAY_SIZE; k++){
		id_array[frame_index][k] = 0;
		object_info[frame_index][k].left = 0;
		object_info[frame_index][k].right = 0;
		object_info[frame_index][k].top = 0;
		object_info[frame_index][k].bot = 0;
		object_info[frame_index][k].class = 0;
		object_info[frame_index][k].id = 99;
	}

	//object tracking
	int now_object_number = 0;

#ifdef CAN
	if(For_Sync){
		if((sw=socket(PF_CAN,SOCK_RAW,CAN_RAW))<0){
			perror("Error while opening socket");
			return -1;
		}
		strcpy(ifr.ifr_name,ifname);
		ioctl(sw, SIOCGIFINDEX,&ifr);
		addr.can_family=AF_CAN;
		addr.can_ifindex=ifr.ifr_ifindex;
		if(bind(sw,(struct sockaddr *)&addr,sizeof(addr))<0) {
			perror("Error in socket bind");
			return -1;
		}
		For_Sync=0;
	}
#endif

	frame.can_dlc = 8;
	//car_cnt cnts;
	cnts->gt_left_cnt = 0;
	cnts->gt_center_cnt = 0;
	cnts->gt_right_cnt = 0;
	cnts->left_cnt = 0;
	cnts->center_cnt = 0;
	cnts->right_cnt = 0;
	cnts->all_left_cnt = 0;
	cnts->all_center_cnt = 0;
	cnts->all_right_cnt = 0;

	if (gt_flag == true){
		// Count GT
		for(int k = 0; k < label_cnt; k++){
			if (atoi(pred_box[k][0]) == 0){
				cnts->gt_left_cnt = 1;
			} else if (atoi(pred_box[k][0]) == 1){
				cnts->gt_center_cnt = 1;
			} else if (atoi(pred_box[k][0]) == 2){
				cnts->gt_right_cnt = 1;
			}
		}
	}
	for(i = 0; i < num; ++i){ //num=nboxes
		char labelstr[4096] = {0};
		int class = -1;
		float confidence;

		for(j = 0; j < classes; ++j){ //classes = demo_classes(80 or 8)
			if (dets[i].prob[j] > thresh){
				if (class < 0) {
					strcat(labelstr, names[j]);
					class = j;
				}
                confidence = dets[i].prob[j];
				printf("%s: %.0f%%\n", names[j], dets[i].prob[j]*100);	
			}
		}


		if(class >= 0){
			int width = im.h * .006;
			int offset = class * 123457 % classes;
			float red = get_color(2, offset,classes);
			float green = get_color(1, offset,classes);
			float blue = get_color(0, offset,classes);
			float rgb[3];

			rgb[0] = red;
			rgb[1] = green;
			rgb[2] = blue;
			box b = dets[i].bbox;

			int left  = (b.x - b.w/2.) * im.w;
			int right = (b.x + b.w/2.) * im.w;
			int top   = (b.y - b.h/2.) * im.h;
			int bot   = (b.y + b.h/2.) * im.h;

			if(left < 0) left = 0;
			if(right > im.w-1) right = im.w - 1;
			if(top < 0) top = 0;
			if(bot > im.h-1) bot = im.h - 1;

			if (strcmp(labelstr, "FVL") == 0 || strcmp(labelstr, "FVI") == 0 || strcmp(labelstr, "FVR") == 0 || strcmp(labelstr, "truck") == 0 || strcmp(labelstr, "bus") == 0){
				char class_name[10] = "vehicle";
            } 
			else if (strcmp(labelstr, "person") == 0){
			}

			if (gt_flag == true){
				bool left_check = false;
				bool center_check = false;
				bool right_check = false;
				for(int k = 0; k < label_cnt; k++){
					if (strcmp(labelstr, "left_car") == 0 && atoi(pred_box[k][0]) == 0){
						if (left_check == false){
							cnts->all_left_cnt = 1;
//							left_check = compute_iou(atof(pred_box[k][1]), atof(pred_box[k][2]), atof(pred_box[k][3]), atof(pred_box[k][4]), left, bot, right, top);
//							if (left_check == true){
//								cnts->left_cnt = 1;
//							}
							if (compute_iou((pred_box + k), left, bot, right, top, atof(pred_box[k][1])) == 1)
								cnts->left_cnt = 1;
						}
					} else if (strcmp(labelstr, "center_car") == 0 && atoi(pred_box[k][0]) == 1){
						if (center_check == false){
							cnts->all_center_cnt = 1;
//							center_check = compute_iou(atof(pred_box[k][1]), atof(pred_box[k][2]), atof(pred_box[k][3]), atof(pred_box[k][4]), left, bot, right, top);
//							if (center_check == true){
//								cnts->center_cnt = 1;
//							}
							if (compute_iou((pred_box + k), left, bot, right, top, atof(pred_box[k][1])) == 1)
								cnts->center_cnt = 1;
						}
					} else if (strcmp(labelstr, "right_car") == 0 && atoi(pred_box[k][0]) == 2){
						if (right_check == false){
							cnts->all_right_cnt = 1;
//							right_check = compute_iou(atof(pred_box[k][1]), atof(pred_box[k][2]), atof(pred_box[k][3]), atof(pred_box[k][4]), left, bot, right, top);
//							if (right_check == true){
//								cnts->right_cnt = 1;
//							}
							if (compute_iou((pred_box + k), left, bot, right, top, atof(pred_box[k][1])) == 1)
								cnts->right_cnt = 1;
						}
					}
				}
			}
			//            exit(0);
			//object tracking start//
			float iou = 0;
			float max_iou = 0.4;
			int need_new_id = 1;	

			for(int k = 0; k < prior_object_number; k++){
				if(object_info[(frame_index + 1) % 2][k].class == class){
					int x1 = __MAX(left, object_info[(frame_index + 1) % 2][k].left);	
					int y1 = __MAX(top, object_info[(frame_index + 1) % 2][k].top);	
					int x2 = __MIN(right, object_info[(frame_index + 1) % 2][k].right);	
					int y2 = __MIN(bot, object_info[(frame_index + 1) % 2][k].bot);	
					int area_intersection = (x2 - x1) * (y2 - y1);
					int area_box1 = (object_info[(frame_index + 1) % 2][k].right - object_info[(frame_index + 1) % 2][k].left) * (object_info[(frame_index + 1) % 2][k].bot - object_info[(frame_index + 1) % 2][k].top);
					int area_box2 = (right - left) * (bot - top);
					iou = (float)area_intersection / (area_box1+area_box2-area_intersection);
					//					printf("now class %d is same with object_info[%d][%d].class %d\n",class,(frame_index+1)%2,k,object_info[(frame_index+1)%2][k].class);
					//					printf("left %d, prior left %d, x1 %d\n",left,object_info[(frame_index+1)%2][k].left,x1);
					//					printf("top %d, prior top %d, y1 %d\n",top,object_info[(frame_index+1)%2][k].top,y1);
					//					printf("right %d, prior right %d, x2 %d\n",right,object_info[(frame_index+1)%2][k].right,x2);
					//					printf("bot %d, prior bot %d, y2 %d\n",bot,object_info[(frame_index+1)%2][k].bot,y2);
					//					printf("x1 %d, y1 %d, x2 %d, y2 %d, iou : %f \n",x1,y1,x2,y2,iou);
					if(iou > max_iou){
						max_iou = iou;
						frame.can_id = object_info[(frame_index + 1) % 2][k].id;
						need_new_id = 0;
						//id_array[frame_index][frame.can_id]=1;
						id_array[frame_index][frame.can_id - 68] = 1;
					}
				}
			}
			if(need_new_id){
				for(int k = 0; k < ARRAY_SIZE; k++){
					if(!id_array[(frame_index + 1) % 2][k])	{
						//frame.can_id=k;
						frame.can_id = k + 68;
						id_array[frame_index][frame.can_id - 68] = 1;
						//id_array[frame_index][frame.can_id]=1;
						id_array[(frame_index + 1) % 2][frame.can_id - 68] = 1;
						//id_array[(frame_index+1)%2][frame.can_id]=1;
						break;
					}
				}
			}
			char _can_id[256];
			sprintf(_can_id,"_%d",frame.can_id-68);
			strcat(labelstr, _can_id);
			//object tracking end//

			int trans_left = left/2;
			int trans_right = right/2;
			int trans_top = top/2;
			int trans_bot = bot/2;

			frame.data[0] = (trans_left << 6) | FVR;
			frame.data[1] = trans_left >> 2;
			frame.data[2] = (trans_top << 7) | FVI;
			frame.data[3] = trans_top >> 1;
			frame.data[4] = ((trans_right - trans_left) << 6) | FVL;
			frame.data[5] = (trans_right - trans_left) >> 2;
			frame.data[6] = ((trans_bot - trans_top) << 7) | (class);
			frame.data[7] = (trans_bot - trans_top) >> 1;

			//write(sw,&frame,sizeof(struct can_frame));
#ifdef CAN
			if(can_send("can0", &frame) == -1) {
				perror("Error CAN send");
				return -1;
			}
#endif

			object_info[frame_index][now_object_number].left = left;			
			object_info[frame_index][now_object_number].right = right;		
			object_info[frame_index][now_object_number].top = top;
			object_info[frame_index][now_object_number].bot = bot;	
			object_info[frame_index][now_object_number].class = class;		
			object_info[frame_index][now_object_number].id = frame.can_id;		
			now_object_number++;

			// SOCKET CAN COMMUNICATION end //

			draw_box_width(im, left, top, right, bot, width, red, green, blue);
			if (alphabet){
				image label = get_label(alphabet, labelstr, (im.h*.03)/10);
				draw_label(im, top + width, left, label, rgb);

				//image label = get_label_v3(alphabet, labelstr, (im.h*.02));
				//draw_weighted_label(im, top + width, left, label, rgb, 0.7);

				free_image(label);
			}
			if (dets[i].mask){
				image mask = float_to_image(14, 14, 1, dets[i].mask);
				image resized_mask = resize_image(mask, b.w*im.w, b.h*im.h);
				image tmask = threshold_image(resized_mask, .5);
				embed_image(tmask, im, left, top);
				free_image(mask);
				free_image(resized_mask);
				free_image(tmask);
			}
			if (box_cnt<20){
				s[box_cnt].tmp_fr=frame_count;
				s[box_cnt].tmp_label=class;
				s[box_cnt].tmp_cenX=(float)(left+right)/2;
				s[box_cnt].tmp_cenY=(float)(top+bot)/2;
				s[box_cnt].tmp_wid=(right-left);
				s[box_cnt].tmp_height=(bot-top);
				s[box_cnt].tmp_conf=confidence;
				box_cnt++;
			}
		} // end(class >=0)
	} // end(i < num(nboxes)) 

    sortCenX(s, 0, box_cnt - 1);
    if(txt_flag){
        for(int k=0; k<box_cnt; k++){
            fprintf(fw1,"%d %d %d %.1f %.1f %d %d %f\n",s[k].tmp_fr,k+1,s[k].tmp_label,s[k].tmp_cenX,s[k].tmp_cenY,s[k].tmp_wid,s[k].tmp_height,s[k].tmp_conf);
        }
        if((20 - box_cnt) > 0){
                for(int i=0; i< (20 - box_cnt); i++){
                    fprintf(fw1,"%d 0 0 0 0 0 0 0\n", frame_count);
                }
        }
        fclose(fw1);
    }

	prior_object_number = now_object_number;

#ifdef CAN
	for(int k=0;k<ARRAY_SIZE;k++){
		if(!id_array[frame_index][k]){

			frame.can_id = k+68;
			//frame.can_id=k;
			frame.can_dlc = 8;
			for(int z=0; z<frame.can_dlc; z++)
				frame.data[z] = 0x00;
			//write(sw,&frame,sizeof(struct can_frame));
			if(can_send("can0", &frame) == -1) {
				perror("Error CAN send");
				return -1;
			}

			usleep(2);
		}
	}
#endif
	frame_index = (frame_index + 1) % 2;

	return 1;
}

void draw_detections_v3(image im, detection *dets, int num, float thresh, char **names, image **alphabet, int classes, int ext_output)
{
    static int frame_id = 0;
    frame_id++;

    int selected_detections_num;
    detection_with_class* selected_detections = get_actual_detections(dets, num, thresh, &selected_detections_num, names);

    // text output
    qsort(selected_detections, selected_detections_num, sizeof(*selected_detections), compare_by_lefts);
    int i;
    for (i = 0; i < selected_detections_num; ++i) {
        const int best_class = selected_detections[i].best_class;
        printf("%s: %.0f%%", names[best_class],    selected_detections[i].det.prob[best_class] * 100);
        if (ext_output)
            printf("\t(left_x: %4.0f   top_y: %4.0f   width: %4.0f   height: %4.0f)\n",
                round((selected_detections[i].det.bbox.x - selected_detections[i].det.bbox.w / 2)*im.w),
                round((selected_detections[i].det.bbox.y - selected_detections[i].det.bbox.h / 2)*im.h),
                round(selected_detections[i].det.bbox.w*im.w), round(selected_detections[i].det.bbox.h*im.h));
        else
            printf("\n");
        int j;
        for (j = 0; j < classes; ++j) {
            if (selected_detections[i].det.prob[j] > thresh && j != best_class) {
                printf("%s: %.0f%%", names[j], selected_detections[i].det.prob[j] * 100);

                if (ext_output)
                    printf("\t(left_x: %4.0f   top_y: %4.0f   width: %4.0f   height: %4.0f)\n",
                        round((selected_detections[i].det.bbox.x - selected_detections[i].det.bbox.w / 2)*im.w),
                        round((selected_detections[i].det.bbox.y - selected_detections[i].det.bbox.h / 2)*im.h),
                        round(selected_detections[i].det.bbox.w*im.w), round(selected_detections[i].det.bbox.h*im.h));
                else
                    printf("\n");
            }
        }
    }

    // image output
    qsort(selected_detections, selected_detections_num, sizeof(*selected_detections), compare_by_probs);
    for (i = 0; i < selected_detections_num; ++i) {
            int width = im.h * .002;
            if (width < 1)
                width = 1;

            /*
            if(0){
            width = pow(prob, 1./2.)*10+1;
            alphabet = 0;
            }
            */

            //printf("%d %s: %.0f%%\n", i, names[selected_detections[i].best_class], prob*100);
            int offset = selected_detections[i].best_class * 123457 % classes;
            float red = get_color(2, offset, classes);
            float green = get_color(1, offset, classes);
            float blue = get_color(0, offset, classes);
            float rgb[3];

            //width = prob*20+2;

            rgb[0] = red;
            rgb[1] = green;
            rgb[2] = blue;
            box b = selected_detections[i].det.bbox;
            //printf("%f %f %f %f\n", b.x, b.y, b.w, b.h);

            int left = (b.x - b.w / 2.)*im.w;
            int right = (b.x + b.w / 2.)*im.w;
            int top = (b.y - b.h / 2.)*im.h;
            int bot = (b.y + b.h / 2.)*im.h;

            if (left < 0) left = 0;
            if (right > im.w - 1) right = im.w - 1;
            if (top < 0) top = 0;
            if (bot > im.h - 1) bot = im.h - 1;

            //int b_x_center = (left + right) / 2;
            //int b_y_center = (top + bot) / 2;
            //int b_width = right - left;
            //int b_height = bot - top;
            //sprintf(labelstr, "%d x %d - w: %d, h: %d", b_x_center, b_y_center, b_width, b_height);

            // you should create directory: result_img
            //static int copied_frame_id = -1;
            //static image copy_img;
            //if (copied_frame_id != frame_id) {
            //    copied_frame_id = frame_id;
            //    if (copy_img.data) free_image(copy_img);
            //    copy_img = copy_image(im);
            //}
            //image cropped_im = crop_image(copy_img, left, top, right - left, bot - top);
            //static int img_id = 0;
            //img_id++;
            //char image_name[1024];
            //int best_class_id = selected_detections[i].best_class;
            //sprintf(image_name, "result_img/img_%d_%d_%d_%s.jpg", frame_id, img_id, best_class_id, names[best_class_id]);
            //save_image(cropped_im, image_name);
            //free_image(cropped_im);

            if (im.c == 1) {
                draw_box_width_bw(im, left, top, right, bot, width, 0.8);    // 1 channel Black-White
            }
            else {
                draw_box_width(im, left, top, right, bot, width, red, green, blue); // 3 channels RGB
            }
            if (alphabet) {
                char labelstr[4096] = { 0 };
                strcat(labelstr, names[selected_detections[i].best_class]);
                char prob_str[10];
                sprintf(prob_str, ": %.2f", selected_detections[i].det.prob[selected_detections[i].best_class]);
                strcat(labelstr, prob_str);
                int j;
                for (j = 0; j < classes; ++j) {
                    if (selected_detections[i].det.prob[j] > thresh && j != selected_detections[i].best_class) {
                        strcat(labelstr, ", ");
                        strcat(labelstr, names[j]);
                    }
                }
                image label = get_label_v3(alphabet, labelstr, (im.h*.02));
                //draw_label(im, top + width, left, label, rgb);
                draw_weighted_label(im, top + width, left, label, rgb, 0.7);
                free_image(label);
            }
            if (selected_detections[i].det.mask) {
                image mask = float_to_image(14, 14, 1, selected_detections[i].det.mask);
                image resized_mask = resize_image(mask, b.w*im.w, b.h*im.h);
                image tmask = threshold_image(resized_mask, .5);
                embed_image(tmask, im, left, top);
                free_image(mask);
                free_image(resized_mask);
                free_image(tmask);
            }
    }
    free(selected_detections);
}

void draw_detections(image im, int num, float thresh, box *boxes, float **probs, char **names, image **alphabet, int classes)
{
    int i;

    for(i = 0; i < num; ++i){
        int class_id = max_index(probs[i], classes);
        float prob = probs[i][class_id];
        if(prob > thresh){

            //// for comparison with OpenCV version of DNN Darknet Yolo v2
            //printf("\n %f, %f, %f, %f, ", boxes[i].x, boxes[i].y, boxes[i].w, boxes[i].h);
            // int k;
            //for (k = 0; k < classes; ++k) {
            //    printf("%f, ", probs[i][k]);
            //}
            //printf("\n");

            int width = im.h * .012;

            if(0){
                width = pow(prob, 1./2.)*10+1;
                alphabet = 0;
            }

            int offset = class_id*123457 % classes;
            float red = get_color(2,offset,classes);
            float green = get_color(1,offset,classes);
            float blue = get_color(0,offset,classes);
            float rgb[3];

            //width = prob*20+2;

            rgb[0] = red;
            rgb[1] = green;
            rgb[2] = blue;
            box b = boxes[i];

            int left  = (b.x-b.w/2.)*im.w;
            int right = (b.x+b.w/2.)*im.w;
            int top   = (b.y-b.h/2.)*im.h;
            int bot   = (b.y+b.h/2.)*im.h;

            if(left < 0) left = 0;
            if(right > im.w-1) right = im.w-1;
            if(top < 0) top = 0;
            if(bot > im.h-1) bot = im.h-1;
            printf("%s: %.0f%%", names[class_id], prob * 100);

            //printf(" - id: %d, x_center: %d, y_center: %d, width: %d, height: %d",
            //    class_id, (right + left) / 2, (bot - top) / 2, right - left, bot - top);

            printf("\n");
            draw_box_width(im, left, top, right, bot, width, red, green, blue);
            if (alphabet) {
                image label = get_label(alphabet, names[class_id], (im.h*.03)/10);
                draw_label(im, top + width, left, label, rgb);
            }
        }
    }
}

void transpose_image(image im)
{
    assert(im.w == im.h);
    int n, m;
    int c;
    for(c = 0; c < im.c; ++c){
        for(n = 0; n < im.w-1; ++n){
            for(m = n + 1; m < im.w; ++m){
                float swap = im.data[m + im.w*(n + im.h*c)];
                im.data[m + im.w*(n + im.h*c)] = im.data[n + im.w*(m + im.h*c)];
                im.data[n + im.w*(m + im.h*c)] = swap;
            }
        }
    }
}

void rotate_image_cw(image im, int times)
{
    assert(im.w == im.h);
    times = (times + 400) % 4;
    int i, x, y, c;
    int n = im.w;
    for(i = 0; i < times; ++i){
        for(c = 0; c < im.c; ++c){
            for(x = 0; x < n/2; ++x){
                for(y = 0; y < (n-1)/2 + 1; ++y){
                    float temp = im.data[y + im.w*(x + im.h*c)];
                    im.data[y + im.w*(x + im.h*c)] = im.data[n-1-x + im.w*(y + im.h*c)];
                    im.data[n-1-x + im.w*(y + im.h*c)] = im.data[n-1-y + im.w*(n-1-x + im.h*c)];
                    im.data[n-1-y + im.w*(n-1-x + im.h*c)] = im.data[x + im.w*(n-1-y + im.h*c)];
                    im.data[x + im.w*(n-1-y + im.h*c)] = temp;
                }
            }
        }
    }
}

void flip_image(image a)
{
    int i,j,k;
    for(k = 0; k < a.c; ++k){
        for(i = 0; i < a.h; ++i){
            for(j = 0; j < a.w/2; ++j){
                int index = j + a.w*(i + a.h*(k));
                int flip = (a.w - j - 1) + a.w*(i + a.h*(k));
                float swap = a.data[flip];
                a.data[flip] = a.data[index];
                a.data[index] = swap;
            }
        }
    }
}

image image_distance(image a, image b)
{
    int i,j;
    image dist = make_image(a.w, a.h, 1);
    for(i = 0; i < a.c; ++i){
        for(j = 0; j < a.h*a.w; ++j){
            dist.data[j] += pow(a.data[i*a.h*a.w+j]-b.data[i*a.h*a.w+j],2);
        }
    }
    for(j = 0; j < a.h*a.w; ++j){
        dist.data[j] = sqrt(dist.data[j]);
    }
    return dist;
}

void embed_image(image source, image dest, int dx, int dy)
{
    int x,y,k;
    for(k = 0; k < source.c; ++k){
        for(y = 0; y < source.h; ++y){
            for(x = 0; x < source.w; ++x){
                float val = get_pixel(source, x,y,k);
                set_pixel(dest, dx+x, dy+y, k, val);
            }
        }
    }
}

image collapse_image_layers(image source, int border)
{
    int h = source.h;
    h = (h+border)*source.c - border;
    image dest = make_image(source.w, h, 1);
    int i;
    for(i = 0; i < source.c; ++i){
        image layer = get_image_layer(source, i);
        int h_offset = i*(source.h+border);
        embed_image(layer, dest, 0, h_offset);
        free_image(layer);
    }
    return dest;
}

void constrain_image(image im)
{
    int i;
    for(i = 0; i < im.w*im.h*im.c; ++i){
        if(im.data[i] < 0) im.data[i] = 0;
        if(im.data[i] > 1) im.data[i] = 1;
    }
}

void normalize_image(image p)
{
    int i;
    float min = 9999999;
    float max = -999999;

    for(i = 0; i < p.h*p.w*p.c; ++i){
        float v = p.data[i];
        if(v < min) min = v;
        if(v > max) max = v;
    }
    if(max - min < .000000001){
        min = 0;
        max = 1;
    }
    for(i = 0; i < p.c*p.w*p.h; ++i){
        p.data[i] = (p.data[i] - min)/(max-min);
    }
}

void normalize_image2(image p)
{
    float* min = (float*)xcalloc(p.c, sizeof(float));
    float* max = (float*)xcalloc(p.c, sizeof(float));
    int i,j;
    for(i = 0; i < p.c; ++i) min[i] = max[i] = p.data[i*p.h*p.w];

    for(j = 0; j < p.c; ++j){
        for(i = 0; i < p.h*p.w; ++i){
            float v = p.data[i+j*p.h*p.w];
            if(v < min[j]) min[j] = v;
            if(v > max[j]) max[j] = v;
        }
    }
    for(i = 0; i < p.c; ++i){
        if(max[i] - min[i] < .000000001){
            min[i] = 0;
            max[i] = 1;
        }
    }
    for(j = 0; j < p.c; ++j){
        for(i = 0; i < p.w*p.h; ++i){
            p.data[i+j*p.h*p.w] = (p.data[i+j*p.h*p.w] - min[j])/(max[j]-min[j]);
        }
    }
    free(min);
    free(max);
}

void copy_image_inplace(image src, image dst)
{
    memcpy(dst.data, src.data, src.h*src.w*src.c * sizeof(float));
}

image copy_image(image p)
{
    image copy = p;
    copy.data = (float*)xcalloc(p.h * p.w * p.c, sizeof(float));
    memcpy(copy.data, p.data, p.h*p.w*p.c*sizeof(float));
    return copy;
}

void rgbgr_image(image im)
{
    int i;
    for(i = 0; i < im.w*im.h; ++i){
        float swap = im.data[i];
        im.data[i] = im.data[i+im.w*im.h*2];
        im.data[i+im.w*im.h*2] = swap;
    }
}

void show_image(image p, const char *name)
{
#ifdef OPENCV
    show_image_cv(p, name);
#else
    fprintf(stderr, "Not compiled with OpenCV, saving to %s.png instead\n", name);
    save_image(p, name);
#endif  // OPENCV
}

void save_image_png(image im, const char *name)
{
    char buff[256];
    //sprintf(buff, "%s (%d)", name, windows);
    sprintf(buff, "%s.png", name);
    unsigned char* data = (unsigned char*)xcalloc(im.w * im.h * im.c, sizeof(unsigned char));
    int i,k;
    for(k = 0; k < im.c; ++k){
        for(i = 0; i < im.w*im.h; ++i){
            data[i*im.c+k] = (unsigned char) (255*im.data[i + k*im.w*im.h]);
        }
    }
    int success = stbi_write_png(buff, im.w, im.h, im.c, data, im.w*im.c);
    free(data);
    if(!success) fprintf(stderr, "Failed to write image %s\n", buff);
}

void save_image_options(image im, const char *name, IMTYPE f, int quality)
{
    char buff[256];
    //sprintf(buff, "%s (%d)", name, windows);
    if (f == PNG)       sprintf(buff, "%s.png", name);
    else if (f == BMP) sprintf(buff, "%s.bmp", name);
    else if (f == TGA) sprintf(buff, "%s.tga", name);
    else if (f == JPG) sprintf(buff, "%s.jpg", name);
    else               sprintf(buff, "%s.png", name);
    unsigned char* data = (unsigned char*)xcalloc(im.w * im.h * im.c, sizeof(unsigned char));
    int i, k;
    for (k = 0; k < im.c; ++k) {
        for (i = 0; i < im.w*im.h; ++i) {
            data[i*im.c + k] = (unsigned char)(255 * im.data[i + k*im.w*im.h]);
        }
    }
    int success = 0;
    if (f == PNG)       success = stbi_write_png(buff, im.w, im.h, im.c, data, im.w*im.c);
    else if (f == BMP) success = stbi_write_bmp(buff, im.w, im.h, im.c, data);
    else if (f == TGA) success = stbi_write_tga(buff, im.w, im.h, im.c, data);
    else if (f == JPG) success = stbi_write_jpg(buff, im.w, im.h, im.c, data, quality);
    free(data);
    if (!success) fprintf(stderr, "Failed to write image %s\n", buff);
}

void save_image(image im, const char *name)
{
    save_image_options(im, name, JPG, 80);
}

void save_image_jpg(image p, const char *name)
{
    save_image_options(p, name, JPG, 80);
}

void show_image_layers(image p, char *name)
{
    int i;
    char buff[256];
    for(i = 0; i < p.c; ++i){
        sprintf(buff, "%s - Layer %d", name, i);
        image layer = get_image_layer(p, i);
        show_image(layer, buff);
        free_image(layer);
    }
}

void show_image_collapsed(image p, char *name)
{
    image c = collapse_image_layers(p, 1);
    show_image(c, name);
    free_image(c);
}

image make_empty_image(int w, int h, int c)
{
    image out;
    out.data = 0;
    out.h = h;
    out.w = w;
    out.c = c;
    return out;
}

image make_image(int w, int h, int c)
{
    image out = make_empty_image(w,h,c);
    out.data = (float*)xcalloc(h * w * c, sizeof(float));
    return out;
}

image make_random_image(int w, int h, int c)
{
    image out = make_empty_image(w,h,c);
    out.data = (float*)xcalloc(h * w * c, sizeof(float));
    int i;
    for(i = 0; i < w*h*c; ++i){
        out.data[i] = (rand_normal() * .25) + .5;
    }
    return out;
}

image float_to_image_scaled(int w, int h, int c, float *data)
{
    image out = make_image(w, h, c);
    int abs_max = 0;
    int i = 0;
    for (i = 0; i < w*h*c; ++i) {
        if (fabs(data[i]) > abs_max) abs_max = fabs(data[i]);
    }
    for (i = 0; i < w*h*c; ++i) {
        out.data[i] = data[i] / abs_max;
    }
    return out;
}

image float_to_image(int w, int h, int c, float *data)
{
    image out = make_empty_image(w,h,c);
    out.data = data;
    return out;
}


image rotate_crop_image(image im, float rad, float s, int w, int h, float dx, float dy, float aspect)
{
    int x, y, c;
    float cx = im.w/2.;
    float cy = im.h/2.;
    image rot = make_image(w, h, im.c);
    for(c = 0; c < im.c; ++c){
        for(y = 0; y < h; ++y){
            for(x = 0; x < w; ++x){
                float rx = cos(rad)*((x - w/2.)/s*aspect + dx/s*aspect) - sin(rad)*((y - h/2.)/s + dy/s) + cx;
                float ry = sin(rad)*((x - w/2.)/s*aspect + dx/s*aspect) + cos(rad)*((y - h/2.)/s + dy/s) + cy;
                float val = bilinear_interpolate(im, rx, ry, c);
                set_pixel(rot, x, y, c, val);
            }
        }
    }
    return rot;
}

image rotate_image(image im, float rad)
{
    int x, y, c;
    float cx = im.w/2.;
    float cy = im.h/2.;
    image rot = make_image(im.w, im.h, im.c);
    for(c = 0; c < im.c; ++c){
        for(y = 0; y < im.h; ++y){
            for(x = 0; x < im.w; ++x){
                float rx = cos(rad)*(x-cx) - sin(rad)*(y-cy) + cx;
                float ry = sin(rad)*(x-cx) + cos(rad)*(y-cy) + cy;
                float val = bilinear_interpolate(im, rx, ry, c);
                set_pixel(rot, x, y, c, val);
            }
        }
    }
    return rot;
}

void translate_image(image m, float s)
{
    int i;
    for(i = 0; i < m.h*m.w*m.c; ++i) m.data[i] += s;
}

void scale_image(image m, float s)
{
    int i;
    for(i = 0; i < m.h*m.w*m.c; ++i) m.data[i] *= s;
}

image crop_image(image im, int dx, int dy, int w, int h)
{
    image cropped = make_image(w, h, im.c);
    int i, j, k;
    for(k = 0; k < im.c; ++k){
        for(j = 0; j < h; ++j){
            for(i = 0; i < w; ++i){
                int r = j + dy;
                int c = i + dx;
                float val = 0;
                r = constrain_int(r, 0, im.h-1);
                c = constrain_int(c, 0, im.w-1);
                if (r >= 0 && r < im.h && c >= 0 && c < im.w) {
                    val = get_pixel(im, c, r, k);
                }
                set_pixel(cropped, i, j, k, val);
            }
        }
    }
    return cropped;
}

int best_3d_shift_r(image a, image b, int min, int max)
{
    if(min == max) return min;
    int mid = floor((min + max) / 2.);
    image c1 = crop_image(b, 0, mid, b.w, b.h);
    image c2 = crop_image(b, 0, mid+1, b.w, b.h);
    float d1 = dist_array(c1.data, a.data, a.w*a.h*a.c, 10);
    float d2 = dist_array(c2.data, a.data, a.w*a.h*a.c, 10);
    free_image(c1);
    free_image(c2);
    if(d1 < d2) return best_3d_shift_r(a, b, min, mid);
    else return best_3d_shift_r(a, b, mid+1, max);
}

int best_3d_shift(image a, image b, int min, int max)
{
    int i;
    int best = 0;
    float best_distance = FLT_MAX;
    for(i = min; i <= max; i += 2){
        image c = crop_image(b, 0, i, b.w, b.h);
        float d = dist_array(c.data, a.data, a.w*a.h*a.c, 100);
        if(d < best_distance){
            best_distance = d;
            best = i;
        }
        printf("%d %f\n", i, d);
        free_image(c);
    }
    return best;
}

void composite_3d(char *f1, char *f2, char *out, int delta)
{
    if(!out) out = "out";
    image a = load_image(f1, 0,0,0);
    image b = load_image(f2, 0,0,0);
    int shift = best_3d_shift_r(a, b, -a.h/100, a.h/100);

    image c1 = crop_image(b, 10, shift, b.w, b.h);
    float d1 = dist_array(c1.data, a.data, a.w*a.h*a.c, 100);
    image c2 = crop_image(b, -10, shift, b.w, b.h);
    float d2 = dist_array(c2.data, a.data, a.w*a.h*a.c, 100);

    if(d2 < d1 && 0){
        image swap = a;
        a = b;
        b = swap;
        shift = -shift;
        printf("swapped, %d\n", shift);
    }
    else{
        printf("%d\n", shift);
    }

    image c = crop_image(b, delta, shift, a.w, a.h);
    int i;
    for(i = 0; i < c.w*c.h; ++i){
        c.data[i] = a.data[i];
    }
#ifdef OPENCV
    save_image_jpg(c, out);
#else
    save_image(c, out);
#endif
}

void fill_image(image m, float s)
{
    int i;
    for (i = 0; i < m.h*m.w*m.c; ++i) m.data[i] = s;
}

void letterbox_image_into(image im, int w, int h, image boxed)
{
    int new_w = im.w;
    int new_h = im.h;
//    if (((float)w / im.w) < ((float)h / im.h)) {
//        new_w = w;
//        new_h = (im.h * w) / im.w;
//    }
//    else {
//        new_h = h;
//        new_w = (im.w * h) / im.h;
//    }
	new_w = w;
	new_h = h;

    image resized = resize_image(im, new_w, new_h);
    embed_image(resized, boxed, (w - new_w) / 2, (h - new_h) / 2);
    free_image(resized);
}

image letterbox_image(image im, int w, int h)
{
    int new_w = im.w;
    int new_h = im.h;
//    if (((float)w / im.w) < ((float)h / im.h)) {
//        new_w = w;
//        new_h = (im.h * w) / im.w;
//    }
//    else {
//        new_h = h;
//        new_w = (im.w * h) / im.h;
//    }
	new_w = w;
	new_h = h;

    image resized = resize_image(im, new_w, new_h);
    image boxed = make_image(w, h, im.c);
    fill_image(boxed, .5);
    //int i;
    //for(i = 0; i < boxed.w*boxed.h*boxed.c; ++i) boxed.data[i] = 0;
    embed_image(resized, boxed, (w - new_w) / 2, (h - new_h) / 2);
    free_image(resized);
    return boxed;
}

image resize_max(image im, int max)
{
    int w = im.w;
    int h = im.h;
    if(w > h){
        h = (h * max) / w;
        w = max;
    } else {
        w = (w * max) / h;
        h = max;
    }
    if(w == im.w && h == im.h) return im;
    image resized = resize_image(im, w, h);
    return resized;
}

image resize_min(image im, int min)
{
    int w = im.w;
    int h = im.h;
    if(w < h){
        h = (h * min) / w;
        w = min;
    } else {
        w = (w * min) / h;
        h = min;
    }
    if(w == im.w && h == im.h) return im;
    image resized = resize_image(im, w, h);
    return resized;
}

image random_crop_image(image im, int w, int h)
{
    int dx = rand_int(0, im.w - w);
    int dy = rand_int(0, im.h - h);
    image crop = crop_image(im, dx, dy, w, h);
    return crop;
}

image random_augment_image(image im, float angle, float aspect, int low, int high, int size)
{
    aspect = rand_scale(aspect);
    int r = rand_int(low, high);
    int min = (im.h < im.w*aspect) ? im.h : im.w*aspect;
    float scale = (float)r / min;

    float rad = rand_uniform(-angle, angle) * 2.0 * M_PI / 360.;

    float dx = (im.w*scale/aspect - size) / 2.;
    float dy = (im.h*scale - size) / 2.;
    if(dx < 0) dx = 0;
    if(dy < 0) dy = 0;
    dx = rand_uniform(-dx, dx);
    dy = rand_uniform(-dy, dy);

    image crop = rotate_crop_image(im, rad, scale, size, size, dx, dy, aspect);

    return crop;
}

float three_way_max(float a, float b, float c)
{
    return (a > b) ? ( (a > c) ? a : c) : ( (b > c) ? b : c) ;
}

float three_way_min(float a, float b, float c)
{
    return (a < b) ? ( (a < c) ? a : c) : ( (b < c) ? b : c) ;
}

// http://www.cs.rit.edu/~ncs/color/t_convert.html
void rgb_to_hsv(image im)
{
    assert(im.c == 3);
    int i, j;
    float r, g, b;
    float h, s, v;
    for(j = 0; j < im.h; ++j){
        for(i = 0; i < im.w; ++i){
            r = get_pixel(im, i , j, 0);
            g = get_pixel(im, i , j, 1);
            b = get_pixel(im, i , j, 2);
            float max = three_way_max(r,g,b);
            float min = three_way_min(r,g,b);
            float delta = max - min;
            v = max;
            if(max == 0){
                s = 0;
                h = 0;
            }else{
                s = delta/max;
                if(r == max){
                    h = (g - b) / delta;
                } else if (g == max) {
                    h = 2 + (b - r) / delta;
                } else {
                    h = 4 + (r - g) / delta;
                }
                if (h < 0) h += 6;
                h = h/6.;
            }
            set_pixel(im, i, j, 0, h);
            set_pixel(im, i, j, 1, s);
            set_pixel(im, i, j, 2, v);
        }
    }
}

void hsv_to_rgb(image im)
{
    assert(im.c == 3);
    int i, j;
    float r, g, b;
    float h, s, v;
    float f, p, q, t;
    for(j = 0; j < im.h; ++j){
        for(i = 0; i < im.w; ++i){
            h = 6 * get_pixel(im, i , j, 0);
            s = get_pixel(im, i , j, 1);
            v = get_pixel(im, i , j, 2);
            if (s == 0) {
                r = g = b = v;
            } else {
                int index = floor(h);
                f = h - index;
                p = v*(1-s);
                q = v*(1-s*f);
                t = v*(1-s*(1-f));
                if(index == 0){
                    r = v; g = t; b = p;
                } else if(index == 1){
                    r = q; g = v; b = p;
                } else if(index == 2){
                    r = p; g = v; b = t;
                } else if(index == 3){
                    r = p; g = q; b = v;
                } else if(index == 4){
                    r = t; g = p; b = v;
                } else {
                    r = v; g = p; b = q;
                }
            }
            set_pixel(im, i, j, 0, r);
            set_pixel(im, i, j, 1, g);
            set_pixel(im, i, j, 2, b);
        }
    }
}

image grayscale_image(image im)
{
    assert(im.c == 3);
    int i, j, k;
    image gray = make_image(im.w, im.h, 1);
    float scale[] = {0.587, 0.299, 0.114};
    for(k = 0; k < im.c; ++k){
        for(j = 0; j < im.h; ++j){
            for(i = 0; i < im.w; ++i){
                gray.data[i+im.w*j] += scale[k]*get_pixel(im, i, j, k);
            }
        }
    }
    return gray;
}

image threshold_image(image im, float thresh)
{
    int i;
    image t = make_image(im.w, im.h, im.c);
    for(i = 0; i < im.w*im.h*im.c; ++i){
        t.data[i] = im.data[i]>thresh ? 1 : 0;
    }
    return t;
}

image blend_image(image fore, image back, float alpha)
{
    assert(fore.w == back.w && fore.h == back.h && fore.c == back.c);
    image blend = make_image(fore.w, fore.h, fore.c);
    int i, j, k;
    for(k = 0; k < fore.c; ++k){
        for(j = 0; j < fore.h; ++j){
            for(i = 0; i < fore.w; ++i){
                float val = alpha * get_pixel(fore, i, j, k) +
                    (1 - alpha)* get_pixel(back, i, j, k);
                set_pixel(blend, i, j, k, val);
            }
        }
    }
    return blend;
}

void scale_image_channel(image im, int c, float v)
{
    int i, j;
    for(j = 0; j < im.h; ++j){
        for(i = 0; i < im.w; ++i){
            float pix = get_pixel(im, i, j, c);
            pix = pix*v;
            set_pixel(im, i, j, c, pix);
        }
    }
}

void translate_image_channel(image im, int c, float v)
{
    int i, j;
    for(j = 0; j < im.h; ++j){
        for(i = 0; i < im.w; ++i){
            float pix = get_pixel(im, i, j, c);
            pix = pix+v;
            set_pixel(im, i, j, c, pix);
        }
    }
}

image binarize_image(image im)
{
    image c = copy_image(im);
    int i;
    for(i = 0; i < im.w * im.h * im.c; ++i){
        if(c.data[i] > .5) c.data[i] = 1;
        else c.data[i] = 0;
    }
    return c;
}

void saturate_image(image im, float sat)
{
    rgb_to_hsv(im);
    scale_image_channel(im, 1, sat);
    hsv_to_rgb(im);
    constrain_image(im);
}

void hue_image(image im, float hue)
{
    rgb_to_hsv(im);
    int i;
    for(i = 0; i < im.w*im.h; ++i){
        im.data[i] = im.data[i] + hue;
        if (im.data[i] > 1) im.data[i] -= 1;
        if (im.data[i] < 0) im.data[i] += 1;
    }
    hsv_to_rgb(im);
    constrain_image(im);
}

void exposure_image(image im, float sat)
{
    rgb_to_hsv(im);
    scale_image_channel(im, 2, sat);
    hsv_to_rgb(im);
    constrain_image(im);
}

void distort_image(image im, float hue, float sat, float val)
{
    if (im.c >= 3)
    {
        rgb_to_hsv(im);
        scale_image_channel(im, 1, sat);
        scale_image_channel(im, 2, val);
        int i;
        for(i = 0; i < im.w*im.h; ++i){
            im.data[i] = im.data[i] + hue;
            if (im.data[i] > 1) im.data[i] -= 1;
            if (im.data[i] < 0) im.data[i] += 1;
        }
        hsv_to_rgb(im);
    }
    else
    {
        scale_image_channel(im, 0, val);
    }
    constrain_image(im);
}

void random_distort_image(image im, float hue, float saturation, float exposure)
{
    float dhue = rand_uniform_strong(-hue, hue);
    float dsat = rand_scale(saturation);
    float dexp = rand_scale(exposure);
    distort_image(im, dhue, dsat, dexp);
}

void saturate_exposure_image(image im, float sat, float exposure)
{
    rgb_to_hsv(im);
    scale_image_channel(im, 1, sat);
    scale_image_channel(im, 2, exposure);
    hsv_to_rgb(im);
    constrain_image(im);
}

float bilinear_interpolate(image im, float x, float y, int c)
{
    int ix = (int) floorf(x);
    int iy = (int) floorf(y);

    float dx = x - ix;
    float dy = y - iy;

    float val = (1-dy) * (1-dx) * get_pixel_extend(im, ix, iy, c) +
        dy     * (1-dx) * get_pixel_extend(im, ix, iy+1, c) +
        (1-dy) *   dx   * get_pixel_extend(im, ix+1, iy, c) +
        dy     *   dx   * get_pixel_extend(im, ix+1, iy+1, c);
    return val;
}

void quantize_image(image im)
{
    int size = im.c * im.w * im.h;
    int i;
    for (i = 0; i < size; ++i) im.data[i] = (int)(im.data[i] * 255) / 255. + (0.5/255);
}

void make_image_red(image im)
{
    int r, c, k;
    for (r = 0; r < im.h; ++r) {
        for (c = 0; c < im.w; ++c) {
            float val = 0;
            for (k = 0; k < im.c; ++k) {
                val += get_pixel(im, c, r, k);
                set_pixel(im, c, r, k, 0);
            }
            for (k = 0; k < im.c; ++k) {
                //set_pixel(im, c, r, k, val);
            }
            set_pixel(im, c, r, 0, val);
        }
    }
}

image make_attention_image(int img_size, float *original_delta_cpu, float *original_input_cpu, int w, int h, int c)
{
    image attention_img;
    attention_img.w = w;
    attention_img.h = h;
    attention_img.c = c;
    attention_img.data = original_delta_cpu;
    make_image_red(attention_img);

    int k;
    float min_val = 999999, mean_val = 0, max_val = -999999;
    for (k = 0; k < img_size; ++k) {
        if (original_delta_cpu[k] < min_val) min_val = original_delta_cpu[k];
        if (original_delta_cpu[k] > max_val) max_val = original_delta_cpu[k];
        mean_val += original_delta_cpu[k];
    }
    mean_val = mean_val / img_size;
    float range = max_val - min_val;

    for (k = 0; k < img_size; ++k) {
        float val = original_delta_cpu[k];
        val = fabs(mean_val - val) / range;
        original_delta_cpu[k] = val * 4;
    }

    image resized = resize_image(attention_img, w / 4, h / 4);
    attention_img = resize_image(resized, w, h);
    free_image(resized);
    for (k = 0; k < img_size; ++k) attention_img.data[k] += original_input_cpu[k];

    //normalize_image(attention_img);
    //show_image(attention_img, "delta");
    return attention_img;
}

image resize_image(image im, int w, int h)
{
    if (im.w == w && im.h == h) return copy_image(im);

    image resized = make_image(w, h, im.c);
    image part = make_image(w, im.h, im.c);
    int r, c, k;
    float w_scale = (float)(im.w - 1) / (w - 1);
    float h_scale = (float)(im.h - 1) / (h - 1);
    for(k = 0; k < im.c; ++k){
        for(r = 0; r < im.h; ++r){
            for(c = 0; c < w; ++c){
                float val = 0;
                if(c == w-1 || im.w == 1){
                    val = get_pixel(im, im.w-1, r, k);
                } else {
                    float sx = c*w_scale;
                    int ix = (int) sx;
                    float dx = sx - ix;
                    val = (1 - dx) * get_pixel(im, ix, r, k) + dx * get_pixel(im, ix+1, r, k);
                }
                set_pixel(part, c, r, k, val);
            }
        }
    }
    for(k = 0; k < im.c; ++k){
        for(r = 0; r < h; ++r){
            float sy = r*h_scale;
            int iy = (int) sy;
            float dy = sy - iy;
            for(c = 0; c < w; ++c){
                float val = (1-dy) * get_pixel(part, c, iy, k);
                set_pixel(resized, c, r, k, val);
            }
            if(r == h-1 || im.h == 1) continue;
            for(c = 0; c < w; ++c){
                float val = dy * get_pixel(part, c, iy+1, k);
                add_pixel(resized, c, r, k, val);
            }
        }
    }

    free_image(part);
    return resized;
}


void test_resize(char *filename)
{
    image im = load_image(filename, 0,0, 3);
    float mag = mag_array(im.data, im.w*im.h*im.c);
    printf("L2 Norm: %f\n", mag);
    image gray = grayscale_image(im);

    image c1 = copy_image(im);
    image c2 = copy_image(im);
    image c3 = copy_image(im);
    image c4 = copy_image(im);
    distort_image(c1, .1, 1.5, 1.5);
    distort_image(c2, -.1, .66666, .66666);
    distort_image(c3, .1, 1.5, .66666);
    distort_image(c4, .1, .66666, 1.5);


    show_image(im,   "Original");
    show_image(gray, "Gray");
    show_image(c1, "C1");
    show_image(c2, "C2");
    show_image(c3, "C3");
    show_image(c4, "C4");

#ifdef OPENCV
    while(1){
        image aug = random_augment_image(im, 0, .75, 320, 448, 320);
        show_image(aug, "aug");
        free_image(aug);


        float exposure = 1.15;
        float saturation = 1.15;
        float hue = .05;

        image c = copy_image(im);

        float dexp = rand_scale(exposure);
        float dsat = rand_scale(saturation);
        float dhue = rand_uniform(-hue, hue);

        distort_image(c, dhue, dsat, dexp);
        show_image(c, "rand");
        printf("%f %f %f\n", dhue, dsat, dexp);
        free_image(c);
        wait_until_press_key_cv();
    }
#endif
}


image load_image_stb(char *filename, int channels)
{
    int w, h, c;
    unsigned char *data = stbi_load(filename, &w, &h, &c, channels);
    if (!data) {
        char shrinked_filename[1024];
        if (strlen(filename) >= 1024) sprintf(shrinked_filename, "name is too long");
        else sprintf(shrinked_filename, "%s", filename);
        fprintf(stderr, "Cannot load image \"%s\"\nSTB Reason: %s\n", shrinked_filename, stbi_failure_reason());
        FILE* fw = fopen("bad.list", "a");
        fwrite(shrinked_filename, sizeof(char), strlen(shrinked_filename), fw);
        char *new_line = "\n";
        fwrite(new_line, sizeof(char), strlen(new_line), fw);
        fclose(fw);
        if (check_mistakes) {
            printf("\n Error in load_image_stb() \n");
            getchar();
        }
        return make_image(10, 10, 3);
        //exit(EXIT_FAILURE);
    }
    if(channels) c = channels;
    int i,j,k;
    image im = make_image(w, h, c);
    for(k = 0; k < c; ++k){
        for(j = 0; j < h; ++j){
            for(i = 0; i < w; ++i){
                int dst_index = i + w*j + w*h*k;
                int src_index = k + c*i + c*w*j;
                im.data[dst_index] = (float)data[src_index]/255.;
            }
        }
    }
    free(data);
    return im;
}

image load_image_stb_resize(char *filename, int w, int h, int c)
{
    image out = load_image_stb(filename, c);    // without OpenCV

    if ((h && w) && (h != out.h || w != out.w)) {
        image resized = resize_image(out, w, h);
        free_image(out);
        out = resized;
    }
    return out;
}

image load_image(char *filename, int w, int h, int c)
{
#ifdef OPENCV
    //image out = load_image_stb(filename, c);
    image out = load_image_cv(filename, c);
#else
    image out = load_image_stb(filename, c);    // without OpenCV
#endif  // OPENCV

    if((h && w) && (h != out.h || w != out.w)){
        image resized = resize_image(out, w, h);
        free_image(out);
        out = resized;
    }
    return out;
}

image load_image_color(char *filename, int w, int h)
{
    return load_image(filename, w, h, 3);
}

image get_image_layer(image m, int l)
{
    image out = make_image(m.w, m.h, 1);
    int i;
    for(i = 0; i < m.h*m.w; ++i){
        out.data[i] = m.data[i+l*m.h*m.w];
    }
    return out;
}

void print_image(image m)
{
    int i, j, k;
    for(i =0 ; i < m.c; ++i){
        for(j =0 ; j < m.h; ++j){
            for(k = 0; k < m.w; ++k){
                printf("%.2lf, ", m.data[i*m.h*m.w + j*m.w + k]);
                if(k > 30) break;
            }
            printf("\n");
            if(j > 30) break;
        }
        printf("\n");
    }
    printf("\n");
}

image collapse_images_vert(image *ims, int n)
{
    int color = 1;
    int border = 1;
    int h,w,c;
    w = ims[0].w;
    h = (ims[0].h + border) * n - border;
    c = ims[0].c;
    if(c != 3 || !color){
        w = (w+border)*c - border;
        c = 1;
    }

    image filters = make_image(w, h, c);
    int i,j;
    for(i = 0; i < n; ++i){
        int h_offset = i*(ims[0].h+border);
        image copy = copy_image(ims[i]);
        //normalize_image(copy);
        if(c == 3 && color){
            embed_image(copy, filters, 0, h_offset);
        }
        else{
            for(j = 0; j < copy.c; ++j){
                int w_offset = j*(ims[0].w+border);
                image layer = get_image_layer(copy, j);
                embed_image(layer, filters, w_offset, h_offset);
                free_image(layer);
            }
        }
        free_image(copy);
    }
    return filters;
}

image collapse_images_horz(image *ims, int n)
{
    int color = 1;
    int border = 1;
    int h,w,c;
    int size = ims[0].h;
    h = size;
    w = (ims[0].w + border) * n - border;
    c = ims[0].c;
    if(c != 3 || !color){
        h = (h+border)*c - border;
        c = 1;
    }

    image filters = make_image(w, h, c);
    int i,j;
    for(i = 0; i < n; ++i){
        int w_offset = i*(size+border);
        image copy = copy_image(ims[i]);
        //normalize_image(copy);
        if(c == 3 && color){
            embed_image(copy, filters, w_offset, 0);
        }
        else{
            for(j = 0; j < copy.c; ++j){
                int h_offset = j*(size+border);
                image layer = get_image_layer(copy, j);
                embed_image(layer, filters, w_offset, h_offset);
                free_image(layer);
            }
        }
        free_image(copy);
    }
    return filters;
}

void show_image_normalized(image im, const char *name)
{
    image c = copy_image(im);
    normalize_image(c);
    show_image(c, name);
    free_image(c);
}

void show_images(image *ims, int n, char *window)
{
    image m = collapse_images_vert(ims, n);
    /*
       int w = 448;
       int h = ((float)m.h/m.w) * 448;
       if(h > 896){
       h = 896;
       w = ((float)m.w/m.h) * 896;
       }
       image sized = resize_image(m, w, h);
     */
    normalize_image(m);
    save_image(m, window);
    show_image(m, window);
    free_image(m);
}

void free_image(image m)
{
    if(m.data){
        free(m.data);
    }
}

// Fast copy data from a contiguous byte array into the image.
LIB_API void copy_image_from_bytes(image im, char *pdata)
{
    unsigned char *data = (unsigned char*)pdata;
    int i, k, j;
    int w = im.w;
    int h = im.h;
    int c = im.c;
    for (k = 0; k < c; ++k) {
        for (j = 0; j < h; ++j) {
            for (i = 0; i < w; ++i) {
                int dst_index = i + w * j + w * h*k;
                int src_index = k + c * i + c * w*j;
                im.data[dst_index] = (float)data[src_index] / 255.;
            }
        }
    }
}
