// main.cpp - 精简版（等价于原 shell 逻辑，已移除异常依赖）
#include <bits/stdc++.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
using namespace std;

const string LOG_FILE = "/data/adb/battery_calibrate.log";
const string BATTERY_PATH = "/sys/class/power_supply/battery";
const string BRIGHTNESS_PATH = "/sys/class/backlight/panel0-backlight";
const string BRIGHTNESS_PATH2 = "/sys/class/leds/lcd-backlight";
const string LCDBRIGHTNESS_PATH = "/sys/devices/platform/soc/soc:mtk_leds/leds/lcd-backlight";
const string COUNTER_FILE = "/data/adb/battery_calibrate.counter";
const string MAX_CHARGE_COUNTER_FILE = "/data/adb/battery_max_charge_counter";
const int MAX_RETRY = 3;
const off_t LOG_MAX_BYTES = 5 * 1024 * 1024;

struct Config { bool enable_monitor=true; bool enable_temp_comp=true; int long_sleep=2; int discharge_threshold=15; } config;

static inline string trim(const string &s){
    size_t a=s.find_first_not_of(" \t\r\n"); if(a==string::npos) return "";
    size_t b=s.find_last_not_of(" \t\r\n"); return s.substr(a,b-a+1);
}
static inline bool file_exists(const string &p){ return access(p.c_str(),F_OK)==0; }
string read_file_trim(const string &p){ ifstream ifs(p); if(!ifs) return ""; string s; getline(ifs,s); return trim(s); }
void write_file_atomic(const string &p,const string &c){ string t=p+".tmp"; ofstream ofs(t,ios::trunc); if(!ofs) return; ofs<<c; ofs.close(); rename(t.c_str(),p.c_str()); }
string now_time(){ time_t t=time(nullptr); char b[64]; strftime(b,sizeof(b),"%Y-%m-%d %H:%M:%S",localtime(&t)); return string(b); }

void log_msg(const string &m){
    struct stat st;
    if(stat(LOG_FILE.c_str(),&st)==0 && st.st_size>=LOG_MAX_BYTES){
        string tmp=LOG_FILE+".tmp";
        string cmd="tail -n 1000 "+LOG_FILE+" > "+tmp+" && mv "+tmp+" "+LOG_FILE;
        system(cmd.c_str());
        ofstream ofs(LOG_FILE,ios::app); if(ofs) ofs<<"["<<now_time()<<"] 日志文件超过5MB，已截断保留最新内容\n";
    }
    ofstream ofs(LOG_FILE,ios::app);
    if(ofs) ofs<<"["<<now_time()<<"] "<<m<<"\n";
}

string exec_cmd(const string &cmd,int &exit_code){
    string full=cmd+" 2>&1";
    FILE *p=popen(full.c_str(),"r");
    if(!p){ exit_code=-1; return "popen failed"; }
    char buf[256]; string out;
    while(fgets(buf,sizeof(buf),p)) out+=buf;
    int st=pclose(p);
    if(st==-1) exit_code=-1; else exit_code = (WIFEXITED(st)?WEXITSTATUS(st):-1);
    return trim(out);
}

bool log_exec(const string &desc,const string &cmd){
    log_msg("正在执行: "+desc+" -> "+cmd);
    for(int i=0;i<MAX_RETRY;i++){
        int code; string out=exec_cmd(cmd,code);
        log_msg("命令输出: "+out);
        if(code==0){ log_msg("执行成功: "+desc); return true; }
        this_thread::sleep_for(chrono::seconds(1));
    }
    log_msg("执行失败 (尝试 "+to_string(MAX_RETRY)+" 次): "+desc);
    return false;
}

long safe_stol_noexcept(const string &s,long def=0){
    if(s.empty()) return def;
    errno=0; char *end=nullptr;
    long v=strtol(s.c_str(),&end,10);
    if(end==s.c_str()||*end!='\0'||errno==ERANGE) return def;
    return v;
}
int safe_stoi_noexcept(const string &s,int def=0){ long v=safe_stol_noexcept(s,def); if(v<INT_MIN) return INT_MIN; if(v>INT_MAX) return INT_MAX; return (int)v; }

void load_config(const string &path){
    ifstream ifs(path);
    if(!ifs){ log_msg("未找到配置文件: "+path+"，使用默认配置"); return; }
    string line;
    while(getline(ifs,line)){
        line=trim(line); if(line.empty()||line[0]=='#') continue;
        auto pos=line.find('='); if(pos==string::npos) continue;
        string k=trim(line.substr(0,pos)), v=trim(line.substr(pos+1));
        if(k=="ENABLE_MONITOR") config.enable_monitor=(v=="true"||v=="1");
        else if(k=="ENABLE_TEMP_COMP") config.enable_temp_comp=(v=="true"||v=="1");
        else if(k=="long_sleep") config.long_sleep=safe_stoi_noexcept(v,config.long_sleep);
        else if(k=="discharge_threshold") config.discharge_threshold=safe_stoi_noexcept(v,config.discharge_threshold);
    }
    log_msg("加载配置: enable_monitor="+string(config.enable_monitor?"true":"false")+
            " enable_temp_comp="+string(config.enable_temp_comp?"true":"false")+
            " long_sleep="+to_string(config.long_sleep)+
            " discharge_threshold="+to_string(config.discharge_threshold));
}

int handle_counter(){
    int cnt=0;
    if(file_exists(COUNTER_FILE)) cnt = safe_stoi_noexcept(read_file_trim(COUNTER_FILE),0);
    cnt++; write_file_atomic(COUNTER_FILE,to_string(cnt));
    log_msg("当前手机重启次数: "+to_string(cnt));
    return cnt;
}

bool cancel_countdown(){
    log_msg("尝试禁用电源服务...");
    string pkg="com.miui.securitycenter/com.miui.powercenter.provider.PowerSaveService";
    if(log_exec("禁用电源服务","pm disable "+pkg)){
        int code; string out=exec_cmd("pm list packages | grep -i \"com.miui.powercenter.provider.PowerSaveService\"",code);
        if(out.empty()){ log_msg("电源服务禁用成功"); return true; }
        log_msg("首次禁用失败，尝试启用再禁用...");
        log_exec("启用电源服务","pm enable "+pkg);
        this_thread::sleep_for(chrono::seconds(5));
        log_exec("再次禁用电源服务","pm disable "+pkg);
        out=exec_cmd("pm list packages | grep -i \"com.miui.powercenter.provider.PowerSaveService\"",code);
        if(out.empty()){ log_msg("电源服务最终禁用成功"); return true; }
        log_msg("电源服务禁用失败"); return false;
    }
    log_msg("禁用电源服务命令执行失败"); return false;
}

void wait_for_batterystats(){
    int total=60, check=60;
    auto start=chrono::steady_clock::now(), last=start;
    log_msg("等待电池服务启动中，需等待1分钟...");
    while(true){
        auto now=chrono::steady_clock::now();
        int elapsed=chrono::duration_cast<chrono::seconds>(now-start).count();
        if(chrono::duration_cast<chrono::seconds>(now-last).count()>=check){
            log_msg("已等待 "+to_string(elapsed/60)+" 分钟，还剩 "+to_string(max(0,(total-elapsed)/60))+" 分钟...");
            last=now;
        }
        if(elapsed>=total){ log_msg("等待电池服务启动完成"); break; }
        this_thread::sleep_for(chrono::seconds(1));
    }
}

bool daemonize(){
    pid_t pid=fork();
    if(pid<0) return false;
    if(pid>0) exit(0);
    if(setsid()<0) return false;
    signal(SIGCHLD,SIG_IGN); signal(SIGHUP,SIG_IGN);
    pid=fork(); if(pid<0) return false; if(pid>0) exit(0);
    umask(0); chdir("/");
    for(int fd=3;fd<64;fd++) close(fd);
    return true;
}

int read_brightness(){
    string p1=BRIGHTNESS_PATH+"/brightness", p2=BRIGHTNESS_PATH2+"/brightness", p3=LCDBRIGHTNESS_PATH+"/brightness";
    if(file_exists(p1)) return (int)safe_stol_noexcept(read_file_trim(p1),0);
    if(file_exists(p2)) return (int)safe_stol_noexcept(read_file_trim(p2),0);
    if(file_exists(p3)) return (int)safe_stol_noexcept(read_file_trim(p3),0);
    return -1;
}

long read_charge_counter_mah(const string &p){
    string raw = read_file_trim(p);
    if(raw.empty()) return 0;
    long v = safe_stol_noexcept(raw,0);
    if(v>20000) return v/1000;
    return v;
}

void monitor_voltage_loop(){
    log_msg("开始电量百分比更新...");
    long max_charge_counter = 0, max_charge_counter_mah = 0, temp_max_charge = 0;
    bool in_full=false; int discharge_counter=0; string last_status="";
    if(file_exists(MAX_CHARGE_COUNTER_FILE)) max_charge_counter = safe_stol_noexcept(read_file_trim(MAX_CHARGE_COUNTER_FILE),0);
    else { max_charge_counter = read_charge_counter_mah(BATTERY_PATH+"/charge_full"); write_file_atomic(MAX_CHARGE_COUNTER_FILE,to_string(max_charge_counter)); }
    max_charge_counter_mah = max_charge_counter;
    log_msg("初次获取最大电池容量: "+to_string(max_charge_counter_mah)+"mAh");

    while(true){
        auto start=chrono::steady_clock::now();
        string charge_raw = read_file_trim(BATTERY_PATH+"/charge_counter");
        long charge_mah = 0;
        if(charge_raw.empty() || charge_raw.find_first_not_of("-0123456789")!=string::npos){
            log_msg("[ERROR] 获取当前电池容量失败: '"+charge_raw+"'");
        } else {
            long raw = safe_stol_noexcept(charge_raw,0);
            charge_mah = (raw>20000? raw/1000 : raw);
        }

        string cap_s = read_file_trim(BATTERY_PATH+"/capacity");
        int capacity = safe_stoi_noexcept(cap_s,0);
        string status = read_file_trim(BATTERY_PATH+"/status");
        if(status.empty()) log_msg("[ERROR] 获取充电状态失败");

        if(status=="Not charging" || status=="Full"){
            if(capacity==100){
                if(!in_full){
                    max_charge_counter = safe_stol_noexcept(charge_raw, max_charge_counter);
                    write_file_atomic(MAX_CHARGE_COUNTER_FILE,to_string(max_charge_counter));
                    temp_max_charge = safe_stol_noexcept(charge_raw,0);
                    in_full=true;
                    max_charge_counter_mah = (max_charge_counter>20000? max_charge_counter/1000 : max_charge_counter);
                    log_msg("电池首次充满，更新最大电池容量:"+to_string(max_charge_counter_mah)+"mAh");
                } else {
                    if(safe_stol_noexcept(charge_raw,0)!=temp_max_charge){
                        max_charge_counter = safe_stol_noexcept(charge_raw,max_charge_counter);
                        temp_max_charge = safe_stol_noexcept(charge_raw,0);
                        write_file_atomic(MAX_CHARGE_COUNTER_FILE,to_string(max_charge_counter));
                        max_charge_counter_mah = (max_charge_counter>20000? max_charge_counter/1000 : max_charge_counter);
                        log_msg("持续充满中，更新最大电池容量:"+to_string(max_charge_counter_mah)+"mAh");
                    }
                }
            } else in_full=false;
        } else in_full=false;

        int brightness = read_brightness();
        if(brightness<0) log_msg("[ERROR] 获取屏幕亮度信息失败");

        if(brightness>0){
            string key = last_status + ":" + status;
            if(key=="Discharging:Charging"){
                log_exec("放电→充电 重置电量","dumpsys battery reset");
                log_msg("放电→充电 | 系统电量:"+to_string(capacity)+"% | 当前电池容量:"+to_string(charge_mah)+"mAh | 最大电池容量:"+to_string(max_charge_counter_mah)+"mAh");
                discharge_counter=0;
            } else if(key=="Charging:Discharging"){
                int level = (max_charge_counter_mah>0? (int)round((double)charge_mah*100.0/(double)max_charge_counter_mah) : 0);
                if(level==0) level=5; if(level>100) level=100;
                log_exec("充电→放电 更新电量","dumpsys battery set level "+to_string(level));
                log_msg("充电→放电 | 更新电量:"+to_string(level)+"% | 系统电量:"+to_string(capacity)+"% | 当前电池容量:"+to_string(charge_mah)+"mAh | 最大电池容量:"+to_string(max_charge_counter_mah)+"mAh");
                discharge_counter=0;
            } else if(key=="Discharging:Discharging"){
                discharge_counter++;
                if((discharge_counter % config.discharge_threshold)==0){
                    int level = (max_charge_counter_mah>0? (int)round((double)charge_mah*100.0/(double)max_charge_counter_mah) : 0);
                    if(level==0) level=5; if(level>100) level=100;
                    log_exec("持续放电 更新电量","dumpsys battery set level "+to_string(level));
                    log_msg("持续放电 | 更新电量:"+to_string(level)+"% | 系统电量:"+to_string(capacity)+"% | 当前电池容量:"+to_string(charge_mah)+"mAh | 最大电池容量:"+to_string(max_charge_counter_mah)+"mAh");
                }
            }
        } else {
            if(last_status=="Discharging" && status=="Charging"){
                log_exec("[息屏] 放电→充电 重置电量","dumpsys battery reset");
                log_msg("[息屏] 放电→充电 | 系统电量:"+to_string(capacity)+"% | 当前电池容量:"+to_string(charge_mah)+"mAh | 最大电池容量:"+to_string(max_charge_counter_mah)+"mAh");
                discharge_counter=0;
            }
        }

        last_status = status;
        auto end = chrono::steady_clock::now();
        int elapsed = (int)chrono::duration_cast<chrono::seconds>(end-start).count();
        int rem = config.long_sleep - elapsed; if(rem<=0) rem=1;
        this_thread::sleep_for(chrono::seconds(rem));
    }
}

int main(int argc,char** argv){
    string cfg="/data/adb/battery_config.ini";
    if(argc>1) cfg=argv[1];
    load_config(cfg);

    if(getuid()!=0){ cerr<<"错误：需要Root权限执行!\n"; log_msg("错误：需要Root权限执行!"); return 1; }
    log_msg("设备信息: 开始执行电池校准程序");

    log_msg("第二步：正在关闭30秒倒计时关机提醒...");
    cancel_countdown();

    log_msg("第三步：正在禁用系统保护机制，重置电池老化因子...");
    if(config.enable_temp_comp) log_exec("禁用温度补偿","setprop persist.vendor.power.disable_temp_comp 1");
    else log_msg("用户未启用'温度补偿'功能，跳过执行。");
    log_exec("禁用电压补偿","setprop persist.vendor.power.disable_voltage_comp 1");
    log_exec("设置老化因子为100","setprop persist.vendor.battery.age_factor 100");

    int reboot_count = handle_counter();
    log_msg("手机重启次数为60的倍数时，才执行\"重置电池统计信息\"");

    if((reboot_count % 60)==0){
        wait_for_batterystats();
        log_exec("重置统计信息","dumpsys batterystats --reset");
        log_exec("发送重置广播","am broadcast -a com.xiaomi.powercenter.RESET_STATS");
        log_exec("删除统计文件","rm /data/system/batterystats.bin");
    }

    log_msg("查询最新配置信息...");
    log_msg("当前电压: "+read_file_trim(BATTERY_PATH+"/voltage_now"));
    log_msg("当前电量: "+read_file_trim(BATTERY_PATH+"/capacity"));
    log_msg("电池健康状态: "+read_file_trim(BATTERY_PATH+"/health"));
    log_msg("电池续航延长操作完成");

    if(config.enable_monitor){
        log_msg("开启'电量百分比更新'功能，程序将转入后台运行。");
        if(!daemonize()){ log_msg("后台守护进程化失败"); return 1; }
        monitor_voltage_loop();
    } else {
        log_msg("'电量百分比更新'功能未启用，核心工作完成，程序退出。");
    }
    return 0;
}
