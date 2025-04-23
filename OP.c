/* OP_optimized.c  —— Chicago Traffic Risk Monitor (资源优化版)
 * 2025-04-24  by ChatGPT
 * 主要优化:
 *   1. 线程池 → 固定 3×采集 + 1×分析 + 1×日志 flush
 *   2. CURL 句柄复用 + 预分配缓冲
 *   3. 条件变量代替 busy-wait; 去掉 100 ms usleep 循环
 *   4. 批量日志 & 增量 GeoJSON
 * 
 * OP.c：真实 API + cJSON 解析 + 动态扩容写回调 + 错误报错
 *        + 几何信息一次性加载 + GeoJSON 导出
 *
 * 交通流量监测系统
 * 1. 启动时拉取道路几何（Overpass）、缓存到内存
 * 2. 定时获取拥堵（模拟）、芝加哥天气（Open-Meteo）、芝加哥车流量（Socrata）
 * 3. 动态扩容 HTTP 响应缓冲，并用 cJSON 解析 JSON
 * 4. 综合评估风险（拥堵、天气、事故率、车流量）、日志记录
 * 5. 导出高风险路段 GeoJSON（LineString），在网页上高亮显示
 *
 * 编译命令（Windows vcpkg）：
    cl /EHsc OP.c \
        /I"%VCPKG_ROOT%\installed\x64-windows\include" \
        /link /LIBPATH:"%VCPKG_ROOT%\installed\x64-windows\lib" \
            cjson.lib \
            libcurl.lib \
            ws2_32.lib \
            crypt32.lib
 * 
 * 编译命令（macOS Homebrew）：
    gcc OP.c -o OP \
      -I/opt/homebrew/include \
      -I/opt/homebrew/include/cjson \
      -L/opt/homebrew/lib \
      -lcjson -lcurl -lpthread
 *
 * 运行本地服务器并查看地图：
 *   python3 -m http.server 8000
 *   浏览器访问 http://localhost:8000/map.html
 */


#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <pthread.h>
#include <unistd.h>
#include <time.h>
#include <string.h>
#include <curl/curl.h>
#include <cjson/cJSON.h>
#include <sys/stat.h>
#include <fcntl.h>

#define MAX_ROADS        100
#define MAX_GEOM_POINTS  200
#define TRAFFIC_IVL      10  // 拥堵更新间隔（秒）
#define WEATHER_IVL      30  // 天气更新间隔（秒）
#define CHICAGO_IVL      10  // 车流量更新间隔（秒）
#define VOLUME_TH        30000.0  // 车流量阈值（辆/小时）
#define LOG_BUF_LINES    256    // 日志缓冲行数

/* ---------- 全局数据 ---------- */
typedef struct {
    int    num_roads;
    double congestion[MAX_ROADS];
    double accident_rate[MAX_ROADS];
    double passing_volume[MAX_ROADS];
    int    geom_len[MAX_ROADS];
    double geom_lon[MAX_ROADS][MAX_GEOM_POINTS];
    double geom_lat[MAX_ROADS][MAX_GEOM_POINTS];
} TrafficData;

typedef struct { int weather_code; } WeatherData;

static TrafficData     gTraffic;
static WeatherData     gWeather;
static int             traffic_ready = 0, weather_ready = 0;
static pthread_mutex_t data_mtx  = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t  ready_cv  = PTHREAD_COND_INITIALIZER;

/* ---------- 日志环形缓冲 ---------- */
static char  log_buf[LOG_BUF_LINES][128];
static size_t log_head = 0, log_tail = 0;
static pthread_mutex_t log_mtx = PTHREAD_MUTEX_INITIALIZER;
static FILE *log_fp;

/* ---------- CURL 帮手 ---------- */
typedef struct {
    char  *data;
    size_t size;
    size_t cap;
} Memory;

static size_t write_callback(void *contents, size_t sz, size_t nm, void *userp) {
    size_t realsz = sz*nm;
    Memory *mem = userp;
    if (mem->size + realsz + 1 > mem->cap) {             // 扩容
        size_t newcap = (mem->size + realsz) * 2;
        char *tmp = realloc(mem->data, newcap);
        if (!tmp) return 0;
        mem->data = tmp; mem->cap = newcap;
    }
    memcpy(mem->data + mem->size, contents, realsz);
    mem->size += realsz;
    mem->data[mem->size] = '\0';
    return realsz;
}

static char *http_get_reuse(CURL *curl, const char *url) {
    Memory mem = { .data = malloc(256), .size = 0, .cap = 256 };
    if (!mem.data) return NULL;
    curl_easy_reset(curl);
    curl_easy_setopt(curl, CURLOPT_URL, url);
    curl_easy_setopt(curl, CURLOPT_FOLLOWLOCATION, 1L);
    curl_easy_setopt(curl, CURLOPT_TIMEOUT, 5L);
    curl_easy_setopt(curl, CURLOPT_WRITEFUNCTION, write_cb);
    curl_easy_setopt(curl, CURLOPT_WRITEDATA, &mem);
    long clen = 0;
    if (curl_easy_perform(curl) != CURLE_OK) { free(mem.data); return NULL; }
    if (curl_easy_getinfo(curl, CURLINFO_CONTENT_LENGTH_DOWNLOAD_T, &clen)==CURLE_OK
        && clen>0 && (size_t)clen+1 > mem.cap) {
        /* 服务器提前告诉了长度但我们没预分配够，极少见，重新 alloc */
    }
    return mem.data;           /* 调用者 free */
}

/* ---------- 日志函数 ---------- */
static void logEvent(const char *fmt, ...) {
    pthread_mutex_lock(&log_mtx);
    va_list ap; va_start(ap,fmt);
    vsnprintf(log_buf[log_head], sizeof log_buf[0], fmt, ap);
    va_end(ap);
    log_head = (log_head + 1) % LOG_BUF_LINES;
    if (log_head == log_tail) {               /* 覆盖最旧 */
        log_tail = (log_tail + 1) % LOG_BUF_LINES;
    }
    pthread_mutex_unlock(&log_mtx);
}

static void *log_flusher(void *arg) {
    log_fp = fopen("system.log","a");
    if (!log_fp) perror("system.log");
    while (1) {
        sleep(1);
        pthread_mutex_lock(&log_mtx);
        while (log_tail != log_head) {
            fputs(log_buf[log_tail], log_fp);
            fputc('\n', log_fp);
            log_tail = (log_tail + 1) % LOG_BUF_LINES;
        }
        fflush(log_fp);
        pthread_mutex_unlock(&log_mtx);
    }
    return NULL;
}

/* ---------- 几何加载 ---------- */
static void load_geometry(CURL *curl) {
    char *resp = http_get_reuse(curl,
      "https://overpass-api.de/api/interpreter"
      "?data=[out:json];way(around:1000,41.8781,-87.6298)[highway];out%20geom;");
    if (!resp) return;
    cJSON *root = cJSON_Parse(resp);
    cJSON *elems = cJSON_GetObjectItem(root,"elements");
    int n = cJSON_IsArray(elems)?cJSON_GetArraySize(elems):0;
    if (n > MAX_ROADS) n = MAX_ROADS;
    pthread_mutex_lock(&data_mtx);
    gTraffic.num_roads = n;
    for (int i=0;i<n;i++) {
        cJSON *it = cJSON_GetArrayItem(elems,i);
        cJSON *geom = cJSON_GetObjectItem(it,"geometry");
        int m = cJSON_IsArray(geom)?cJSON_GetArraySize(geom):0;
        if (m>MAX_GEOM_POINTS) m = MAX_GEOM_POINTS;
        gTraffic.geom_len[i]=m;
        for (int j=0;j<m;j++){
            cJSON *pt=cJSON_GetArrayItem(geom,j);
            gTraffic.geom_lat[i][j]=cJSON_GetObjectItem(pt,"lat")->valuedouble;
            gTraffic.geom_lon[i][j]=cJSON_GetObjectItem(pt,"lon")->valuedouble;
        }
    }
    pthread_mutex_unlock(&data_mtx);
    cJSON_Delete(root); free(resp);
}

/* ---------- 采集线程 ---------- */
static void *fetchTraffic(void *arg){
    CURL *curl=curl_easy_init();         /* 每线程私有 */
    struct timespec ivl={TRAFFIC_IVL,0};
    while (1){
        nanosleep(&ivl,NULL);
        pthread_mutex_lock(&data_mtx);
        for(int i=0;i<gTraffic.num_roads;i++)
            gTraffic.congestion[i]=rand()%10;
        traffic_ready=1;
        pthread_cond_signal(&ready_cv);
        pthread_mutex_unlock(&data_mtx);
    }
}

static void *fetchWeather(void *arg){
    CURL *curl=curl_easy_init();
    struct timespec ivl={WEATHER_IVL,0};
    const char *url=
      "https://api.open-meteo.com/v1/forecast?latitude=41.8781&longitude=-87.6298"
      "&current_weather=true&timezone=America%2FChicago";
    while(1){
        nanosleep(&ivl,NULL);
        char *resp = http_get_reuse(curl,url);
        if(!resp) continue;
        int rain = (strstr(resp,"\"weathercode\":61")||strstr(resp,"rain"));
        pthread_mutex_lock(&data_mtx);
        gWeather.weather_code = rain?1:0;
        weather_ready=1;
        pthread_cond_signal(&ready_cv);
        pthread_mutex_unlock(&data_mtx);
        free(resp);
    }
}

static void *fetchChicagoVolume(void *arg){
    CURL *curl=curl_easy_init();
    const char *url=
      "https://data.cityofchicago.org/resource/u77m-8jgp.json?"
      "$where=total_passing_vehicle_volume>20000";
    struct timespec ivl={CHICAGO_IVL,0};
    while(1){
        nanosleep(&ivl,NULL);
        char *resp = http_get_reuse(curl,url);
        if(!resp) continue;
        cJSON *root=cJSON_Parse(resp);
        int cnt=(root&&cJSON_IsArray(root))?cJSON_GetArraySize(root):0;
        if(cnt>gTraffic.num_roads) cnt=gTraffic.num_roads;
        pthread_mutex_lock(&data_mtx);
        for(int i=0;i<cnt;i++){
            cJSON *item=cJSON_GetArrayItem(root,i);
            cJSON *vol=cJSON_GetObjectItem(item,"total_passing_vehicle_volume");
            gTraffic.passing_volume[i]=
              cJSON_IsNumber(vol)?vol->valuedouble:atof(vol->valuestring);
        }
        pthread_mutex_unlock(&data_mtx);
        cJSON_Delete(root); free(resp);
    }
}

/* ---------- GeoJSON 导出（增量） ---------- */
static double last_export_risk[MAX_ROADS]={0};
static void exportRiskGeoJSON(void){
    int changed=0;
    for(int i=0;i<gTraffic.num_roads;i++){
        double risk=0;
        if(gTraffic.congestion[i]>=8) risk+=.5;
        if(gWeather.weather_code==1)  risk+=.3;
        if(gTraffic.accident_rate[i]>.01) risk+=.2;
        if(gTraffic.passing_volume[i]>VOLUME_TH) risk+=.2;
        if(risk!=last_export_risk[i]){changed=1; last_export_risk[i]=risk;}
    }
    if(!changed) return;
    FILE *f=fopen("risky_roads.geojson.tmp","w");
    if(!f) return;
    fprintf(f,"{\"type\":\"FeatureCollection\",\"features\":[");
    int first=1;
    for(int i=0;i<gTraffic.num_roads;i++){
        double risk=last_export_risk[i];
        if(risk<0.7||gTraffic.geom_len[i]<2) continue;
        if(!first) fputc(',',f); else first=0;
        fprintf(f,"{\"type\":\"Feature\",\"properties\":{\"risk\":%.2f},\"geometry\":{\"type\":\"LineString\",\"coordinates\":[",risk);
        for(int j=0;j<gTraffic.geom_len[i];j++){
            fprintf(f,"[%.6f,%.6f]",gTraffic.geom_lon[i][j],gTraffic.geom_lat[i][j]);
            if(j+1<gTraffic.geom_len[i]) fputc(',',f);
        }
        fputs("]}}",f);
    }
    fputs("]}",f);
    fclose(f);
    rename("risky_roads.geojson.tmp","risky_roads.geojson");
}

/* ---------- 分析线程 ---------- */
static void *analyzeRisk(void *arg){
    while(1){
        pthread_mutex_lock(&data_mtx);
        while(!(traffic_ready&&weather_ready))
            pthread_cond_wait(&ready_cv,&data_mtx);
        traffic_ready=weather_ready=0;

        /* 快照以减少持锁时间 */
        TrafficData t=gTraffic; WeatherData w=gWeather;
        pthread_mutex_unlock(&data_mtx);

        for(int i=0;i<t.num_roads;i++){
            double risk=0;
            if(t.congestion[i]>=8) risk+=.5;
            if(w.weather_code==1)  risk+=.3;
            if(t.accident_rate[i]>.01) risk+=.2;
            if(t.passing_volume[i]>VOLUME_TH) risk+=.2;
            if(risk>=0.7){
                printf("[ALERT] Road %d HIGH RISK %.2f\n",i,risk);
                log_enqueue("[%ld] HighRisk: Road%d = %.2f",time(NULL),i,risk);
            }
        }
        /* 按需导出 geojson */
        pthread_mutex_lock(&data_mtx);
        export_geojson_if_changed();
        pthread_mutex_unlock(&data_mtx);
    }
}

/* ---------- 初始化 ---------- */
static void initialize(void){
    for(int i=0;i<MAX_ROADS;i++)
        gTraffic.accident_rate[i]=(double)rand()/RAND_MAX*0.05;
}

int main(){
    srand(time(NULL));
    curl_global_init(CURL_GLOBAL_ALL);

    /* 先加载静态几何 */
    CURL *one=curl_easy_init();
    load_geometry(one); curl_easy_cleanup(one);
    init_random_accident();

    /* 启动线程 */
    pthread_t tid_traffic,tid_weather,tid_chicago,tid_analyze,tid_log;
    pthread_create(&tid_traffic ,NULL,traffic_worker ,NULL);
    pthread_create(&tid_weather ,NULL,weather_worker ,NULL);
    pthread_create(&tid_chicago ,NULL,chicago_worker ,NULL);
    pthread_create(&tid_analyze ,NULL,analyze_worker ,NULL);
    pthread_create(&tid_log     ,NULL,log_flusher    ,NULL);

    /* 主线程阻塞等待 */
    pthread_join(tid_analyze,NULL);
    return 0;
}
