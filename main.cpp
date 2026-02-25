// main.cpp
#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <cerrno>
#include <ctime>
#include <vector>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>

using namespace std;

// --- 常量（与脚本路径一致） ---
const string LOG_FILE = "/data/adb/battery_calibrate.log";
const string BATTERY_PATH = "/sys/class/power_supply/battery";
const string BRIGHTNESS_PATH = "/sys/class/backlight/panel0-backlight";
const string BRIGHTNESS_PATH2 = "/sys/class/leds/lcd-backlight";
const string LCDBRIGHTNESS_PATH = "/sys/devices/platform/soc/soc:mtk_leds/leds/lcd-backlight";
const string COUNTER_FILE = "/data/adb/battery_calibrate.counter";
const string MAX_CHARGE_COUNTER_FILE = "/data/adb/battery_max_charge_counter";
const int MAX_RETRY = 3;
const off_t LOG_MAX_BYTES = 5 * 1024 * 1024; // 5MB

// --- 配置结构体（可由外部文件覆盖） ---
struct Config {
    bool enable_monitor = true;
    bool enable_temp_comp = true;
    int long_sleep = 2;
    int discharge_threshold = 15;
} config;

// --- 工具函数 ---
string trim(const string& s) {
    size_t a = s.find_first_not_of(" \t\r\n");
    if (a == string::npos) return "";
    size_t b = s.find_last_not_of(" \t\r\n");
    return s.substr(a, b - a + 1);
}

bool file_exists(const string& path) {
    return access(path.c_str(), F_OK) == 0;
}

string read_file_trim(const string& path) {
    ifstream ifs(path);
    if (!ifs.is_open()) return "";
    string s;
    getline(ifs, s);
    return trim(s);
}

void write_file_atomic(const string& path, const string& content) {
    string tmp = path + ".tmp";
    ofstream ofs(tmp, ios::trunc);
    if (!ofs.is_open()) return;
    ofs << content;
    ofs.close();
    rename(tmp.c_str(), path.c_str());
}

string get_time() {
    auto now = chrono::system_clock::now();
    time_t t = chrono::system_clock::to_time_t(now);
    tm tm = *localtime(&t);
    ostringstream oss;
    oss << put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// 日志（带轮转）
void log_msg(const string& msg) {
    // 检查并截断日志
    struct stat st;
    if (stat(LOG_FILE.c_str(), &st) == 0 && st.st_size >= LOG_MAX_BYTES) {
        // 保留最后 1000 行
        string tmp = LOG_FILE + ".tmp";
        // 使用 shell tail 简化实现（设备上通常可用）
        string cmd = "tail -n 1000 " + LOG_FILE + " > " + tmp + " && mv " + tmp + " " + LOG_FILE;
        system(cmd.c_str());
        ofstream ofs(LOG_FILE, ios::app);
        if (ofs.is_open()) {
            ofs << "[" << get_time() << "] 日志文件超过5MB，已截断保留最新内容\n";
            ofs.close();
        }
    }
    ofstream ofs(LOG_FILE, ios::app);
    if (ofs.is_open()) {
        ofs << "[" << get_time() << "] " << msg << "\n";
    }
}

// 执行命令并返回输出与退出码
string exec_cmd(const string& cmd, int &exit_code) {
    string full = cmd + " 2>&1";
    FILE* pipe = popen(full.c_str(), "r");
    if (!pipe) { exit_code = -1; return "popen failed"; }
    char buf[256];
    string out;
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
    int status = pclose(pipe);
    if (status == -1) exit_code = -1;
    else {
        if (WIFEXITED(status)) exit_code = WEXITSTATUS(status);
        else exit_code = -1;
    }
    return trim(out);
}

// 带重试的命令执行（记录日志）
bool log_exec(const string& desc, const string& cmd) {
    log_msg("正在执行: " + desc + " -> " + cmd);
    int retry = 0;
    while (retry < MAX_RETRY) {
        int code;
        string out = exec_cmd(cmd, code);
        log_msg("命令输出: " + out);
        if (code == 0) {
            log_msg("执行成功: " + desc);
            return true;
        }
        retry++;
        this_thread::sleep_for(chrono::seconds(1));
    }
    log_msg("执行失败 (尝试 " + to_string(MAX_RETRY) + " 次): " + desc);
    return false;
}

// 读取配置文件
void load_config(const string& path) {
    ifstream ifs(path);
    if (!ifs.is_open()) {
        log_msg("未找到配置文件: " + path + "，使用默认配置");
        return;
    }
    string line;
    while (getline(ifs, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t pos = line.find('=');
        if (pos == string::npos) continue;
        string key = trim(line.substr(0, pos));
        string val = trim(line.substr(pos + 1));
        try {
            if (key == "ENABLE_MONITOR") config.enable_monitor = (val == "true" || val == "1");
            else if (key == "ENABLE_TEMP_COMP") config.enable_temp_comp = (val == "true" || val == "1");
            else if (key == "long_sleep") config.long_sleep = stoi(val);
            else if (key == "discharge_threshold") config.discharge_threshold = stoi(val);
        } catch (...) {
            log_msg("配置解析错误: " + key + "=" + val);
        }
    }
    log_msg("加载配置: enable_monitor=" + string(config.enable_monitor ? "true":"false")
            + " enable_temp_comp=" + string(config.enable_temp_comp ? "true":"false")
            + " long_sleep=" + to_string(config.long_sleep)
            + " discharge_threshold=" + to_string(config.discharge_threshold));
}

// 计数器处理
int handle_counter() {
    int reboot_count = 0;
    if (file_exists(COUNTER_FILE)) {
        string s = read_file_trim(COUNTER_FILE);
        try { reboot_count = stoi(s); } catch(...) { reboot_count = 0; }
    }
    reboot_count++;
    write_file_atomic(COUNTER_FILE, to_string(reboot_count));
    log_msg("当前手机重启次数: " + to_string(reboot_count));
    return reboot_count;
}

// 取消 30 秒倒计时提醒（移植 cancel_countdown）
bool cancel_countdown() {
    log_msg("尝试禁用电源服务...");
    // 目标 service 名称与脚本一致
    string pkg = "com.miui.securitycenter/com.miui.powercenter.provider.PowerSaveService";
    if (log_exec("禁用电源服务", "pm disable " + pkg)) {
        // 检查是否禁用成功
        int code; string out = exec_cmd("pm list packages | grep -i \"com.miui.powercenter.provider.PowerSaveService\"", code);
        if (out.empty()) {
            log_msg("电源服务禁用成功");
            return true;
        } else {
            log_msg("首次禁用失败，尝试启用再禁用...");
            log_exec("启用电源服务", "pm enable " + pkg);
            this_thread::sleep_for(chrono::seconds(5));
            log_exec("再次禁用电源服务", "pm disable " + pkg);
            out = exec_cmd("pm list packages | grep -i \"com.miui.powercenter.provider.PowerSaveService\"", code);
            if (out.empty()) {
                log_msg("电源服务最终禁用成功");
                return true;
            } else {
                log_msg("电源服务禁用失败");
                return false;
            }
        }
    } else {
        log_msg("禁用电源服务命令执行失败");
        return false;
    }
}

// 等待电池服务启动（wait_for_batterystats）
void wait_for_batterystats() {
    int total_timeout = 60;
    int check_interval = 60;
    auto start = chrono::steady_clock::now();
    auto last_log = start;
    log_msg("等待电池服务启动中，需等待1分钟...");
    while (true) {
        auto now = chrono::steady_clock::now();
        int elapsed = chrono::duration_cast<chrono::seconds>(now - start).count();
        int remaining = total_timeout - elapsed;
        if (chrono::duration_cast<chrono::seconds>(now - last_log).count() >= check_interval) {
            log_msg("已等待 " + to_string(elapsed/60) + " 分钟，还剩 " + to_string(max(0, remaining/60)) + " 分钟...");
            last_log = now;
        }
        if (elapsed >= total_timeout) {
            log_msg("等待电池服务启动完成");
            break;
        }
        this_thread::sleep_for(chrono::seconds(1));
    }
}

// 守护化（双 fork）
bool daemonize() {
    pid_t pid = fork();
    if (pid < 0) return false;
    if (pid > 0) exit(0); // parent exit
    if (setsid() < 0) return false;
    signal(SIGCHLD, SIG_IGN);
    signal(SIGHUP, SIG_IGN);
    pid = fork();
    if (pid < 0) return false;
    if (pid > 0) exit(0);
    umask(0);
    chdir("/");
    // 关闭不必要的 fd
    for (int fd = 3; fd < 64; ++fd) close(fd);
    return true;
}

// 将字符串解析为整数（安全）
long safe_stol(const string& s) {
    try { return stol(s); } catch(...) { return 0; }
}

// 读取亮度（多路径）
int read_brightness() {
    string p1 = BRIGHTNESS_PATH + string("/brightness");
    string p2 = BRIGHTNESS_PATH2 + string("/brightness");
    string p3 = LCDBRIGHTNESS_PATH + string("/brightness");
    if (file_exists(p1)) return (int)safe_stol(read_file_trim(p1));
    if (file_exists(p2)) return (int)safe_stol(read_file_trim(p2));
    if (file_exists(p3)) return (int)safe_stol(read_file_trim(p3));
    return -1;
}

// 读取 charge_counter 与 charge_full 并做单位换算（返回 mAh）
long read_charge_counter_mah(const string& path) {
    string raw = read_file_trim(path);
    if (raw.empty()) return 0;
    long v = safe_stol(raw);
    // 脚本逻辑：如果值大于20000，认为单位是 uAh，需要 /1000
    if (v > 20000) return v / 1000;
    return v;
}

// monitor_voltage 完整实现（与脚本等价）
void monitor_voltage_loop() {
    log_msg("开始电量百分比更新...");
    long max_charge_counter = 0;
    long max_charge_counter_mah = 0;
    bool in_full_state = false;
    long temp_max_charge = 0;
    int discharge_counter = 0;
    string last_status = "";
    // 初次获取最大电池容量
    if (file_exists(MAX_CHARGE_COUNTER_FILE)) {
        max_charge_counter = safe_stol(read_file_trim(MAX_CHARGE_COUNTER_FILE));
    } else {
        max_charge_counter = read_charge_counter_mah(BATTERY_PATH + string("/charge_full"));
        write_file_atomic(MAX_CHARGE_COUNTER_FILE, to_string(max_charge_counter));
    }
    max_charge_counter_mah = max_charge_counter;
    log_msg("初次获取最大电池容量: " + to_string(max_charge_counter_mah) + "mAh");

    while (true) {
        auto loop_start = chrono::steady_clock::now();

        // 读取当前 charge_counter（原始）
        string charge_counter_raw = read_file_trim(BATTERY_PATH + string("/charge_counter"));
        long charge_counter_mah = 0;
        if (charge_counter_raw.empty() || charge_counter_raw.find_first_not_of("-0123456789") != string::npos) {
            log_msg("[ERROR] 获取当前电池容量失败: 读取到 '" + charge_counter_raw + "'");
        } else {
            long raw = safe_stol(charge_counter_raw);
            // 单位判断：如果 raw > 20000 -> uAh
            if (raw > 20000) charge_counter_mah = raw / 1000;
            else charge_counter_mah = raw;
        }

        // 获取系统电量百分比
        string capacity_s = read_file_trim(BATTERY_PATH + string("/capacity"));
        int capacity = 0;
        if (capacity_s.empty() || capacity_s.find_first_not_of("-0123456789") != string::npos) {
            log_msg("[ERROR] 获取系统电量百分比失败: 读取到 '" + capacity_s + "'");
        } else {
            capacity = (int)safe_stol(capacity_s);
        }

        // 获取充电状态
        string charging_status = read_file_trim(BATTERY_PATH + string("/status"));
        if (charging_status.empty()) {
            log_msg("[ERROR] 获取充电状态失败: 读取到空");
        }

        // 更新最大电池容量逻辑（与脚本一致）
        if (charging_status == "Not charging" || charging_status == "Full") {
            if (capacity == 100) {
                if (!in_full_state) {
                    max_charge_counter = safe_stol(charge_counter_raw);
                    write_file_atomic(MAX_CHARGE_COUNTER_FILE, to_string(max_charge_counter));
                    temp_max_charge = safe_stol(charge_counter_raw);
                    in_full_state = true;
                    if (max_charge_counter > 20000) max_charge_counter_mah = max_charge_counter / 1000;
                    else max_charge_counter_mah = max_charge_counter;
                    log_msg("电池首次充满，更新最大电池容量:" + to_string(max_charge_counter_mah) + "mAh");
                } else {
                    if (safe_stol(charge_counter_raw) != temp_max_charge) {
                        max_charge_counter = safe_stol(charge_counter_raw);
                        temp_max_charge = safe_stol(charge_counter_raw);
                        write_file_atomic(MAX_CHARGE_COUNTER_FILE, to_string(max_charge_counter));
                        if (max_charge_counter > 20000) max_charge_counter_mah = max_charge_counter / 1000;
                        else max_charge_counter_mah = max_charge_counter;
                        log_msg("持续充满中，更新最大电池容量:" + to_string(max_charge_counter_mah) + "mAh");
                    } else {
                        // 维持最大电池容量（不频繁写日志）
                    }
                }
            } else {
                in_full_state = false;
            }
        } else {
            in_full_state = false;
        }

        // 获取亮度
        int brightness = read_brightness();
        if (brightness < 0) {
            log_msg("[ERROR] 获取屏幕亮度信息失败");
        }

        // 状态机处理（与脚本一致）
        if (brightness > 0) {
            string key = last_status + ":" + charging_status;
            if (key == "Discharging:Charging") {
                // 放电->充电
                log_exec("放电→充电 重置电量", "dumpsys battery reset");
                log_msg("放电→充电 | 系统电量:" + to_string(capacity) + "% | 当前电池容量:" + to_string(charge_counter_mah) + "mAh | 最大电池容量:" + to_string(max_charge_counter_mah) + "mAh");
                discharge_counter = 0;
            } else if (key == "Charging:Discharging") {
                // 充电->放电
                int level = 0;
                if (max_charge_counter_mah > 0) level = (int)round((double)charge_counter_mah * 100.0 / (double)max_charge_counter_mah);
                if (level == 0) level = 5;
                if (level > 100) level = 100;
                log_exec("充电→放电 更新电量", "dumpsys battery set level " + to_string(level));
                log_msg("充电→放电 | 更新电量:" + to_string(level) + "% | 系统电量:" + to_string(capacity) + "% | 当前电池容量:" + to_string(charge_counter_mah) + "mAh | 最大电池容量:" + to_string(max_charge_counter_mah) + "mAh");
                discharge_counter = 0;
            } else if (key == "Charging:Charging") {
                // 持续充电，不做额外操作
            } else if (key == "Discharging:Discharging") {
                discharge_counter++;
                if ((discharge_counter % config.discharge_threshold) == 0) {
                    int level = 0;
                    if (max_charge_counter_mah > 0) level = (int)round((double)charge_counter_mah * 100.0 / (double)max_charge_counter_mah);
                    if (level == 0) level = 5;
                    if (level > 100) level = 100;
                    log_exec("持续放电 更新电量", "dumpsys battery set level " + to_string(level));
                    log_msg("持续放电 | 更新电量:" + to_string(level) + "% | 系统电量:" + to_string(capacity) + "% | 当前电池容量:" + to_string(charge_counter_mah) + "mAh | 最大电池容量:" + to_string(max_charge_counter_mah) + "mAh");
                }
            } else {
                // 其他组合，首次进入或未知状态
            }
        } else {
            // 屏幕熄灭
            if (last_status == "Discharging" && charging_status == "Charging") {
                log_exec("[息屏] 放电→充电 重置电量", "dumpsys battery reset");
                log_msg("[息屏] 放电→充电 | 系统电量:" + to_string(capacity) + "% | 当前电池容量:" + to_string(charge_counter_mah) + "mAh | 最大电池容量:" + to_string(max_charge_counter_mah) + "mAh");
                discharge_counter = 0;
            } else {
                // 屏幕熄灭，跳过本轮更新
            }
        }

        last_status = charging_status;

        // 计算等待时间
        auto loop_end = chrono::steady_clock::now();
        int elapsed = (int)chrono::duration_cast<chrono::seconds>(loop_end - loop_start).count();
        int remaining = config.long_sleep - elapsed;
        if (remaining <= 0) remaining = 1;
        this_thread::sleep_for(chrono::seconds(remaining));
    }
}

// 主流程
int main(int argc, char* argv[]) {
    string config_path = "/data/adb/battery_config.ini";
    if (argc > 1) config_path = argv[1];
    load_config(config_path);

    // Root 检查
    if (getuid() != 0) {
        cerr << "错误：需要Root权限执行!" << endl;
        log_msg("错误：需要Root权限执行!");
        return 1;
    }
    log_msg("设备信息: 开始执行电池校准程序");

    // 关闭 30 秒倒计时
    log_msg("第二步：正在关闭30秒倒计时关机提醒...");
    cancel_countdown();

    // 禁用系统保护机制（根据配置）
    log_msg("第三步：正在禁用系统保护机制，重置电池老化因子...");
    if (config.enable_temp_comp) {
        log_exec("禁用温度补偿", "setprop persist.vendor.power.disable_temp_comp 1");
    } else {
        log_msg("用户未启用'温度补偿'功能，跳过执行。");
    }
    log_exec("禁用电压补偿", "setprop persist.vendor.power.disable_voltage_comp 1");
    log_exec("设置老化因子为100", "setprop persist.vendor.battery.age_factor 100");

    // 计数器处理
    int reboot_count = handle_counter();
    log_msg("手机重启次数为60的倍数时，才执行\"重置电池统计信息\"");

    if ((reboot_count % 60) == 0) {
        log_exec("等待电池服务", "true"); // 仅记录，实际等待用函数
        wait_for_batterystats();
        log_exec("重置统计信息", "dumpsys batterystats --reset");
        log_exec("发送重置广播", "am broadcast -a com.xiaomi.powercenter.RESET_STATS");
        log_exec("删除统计文件", "rm /data/system/batterystats.bin");
    }

    // 查询最新配置信息并记录
    string voltage_now = read_file_trim(BATTERY_PATH + string("/voltage_now"));
    string capacity = read_file_trim(BATTERY_PATH + string("/capacity"));
    string health = read_file_trim(BATTERY_PATH + string("/health"));
    log_msg("当前电压: " + voltage_now);
    log_msg("当前电量: " + capacity);
    log_msg("电池健康状态: " + health);
    log_msg("电池续航延长操作完成");

    // 电量百分比更新（根据配置）
    if (config.enable_monitor) {
        log_msg("开启'电量百分比更新'功能，程序将转入后台运行。");
        if (!daemonize()) {
            log_msg("后台守护进程化失败");
            return 1;
        }
        // 后台运行 monitor
        monitor_voltage_loop();
    } else {
        log_msg("用户未启用'电量百分比更新'功能，核心工作完成，程序退出。");
    }

    return 0;
}
