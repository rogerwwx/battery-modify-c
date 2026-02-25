#include <iostream>
#include <fstream>
#include <string>
#include <chrono>
#include <thread>
#include <sstream>
#include <iomanip>
#include <cstdio>
#include <cstdlib>
#include <unistd.h>
#include <sys/stat.h>
#include <sys/wait.h>

// --- 写死在程序中的路径常量 ---
const std::string LOG_FILE = "/data/adb/battery_calibrate.log";
const std::string BATTERY_PATH = "/sys/class/power_supply/battery";
const std::string BRIGHTNESS_PATH = "/sys/class/backlight/panel0-backlight/";
const std::string BRIGHTNESS_PATH2 = "/sys/class/leds/lcd-backlight/";
const std::string LCDBRIGHTNESS_PATH = "/sys/devices/platform/soc/soc:mtk_leds/leds/lcd-backlight/";
const std::string COUNTER_FILE = "/data/adb/battery_calibrate.counter";
const std::string MAX_CHARGE_COUNTER_FILE = "/data/adb/battery_max_charge_counter";
const int MAX_RETRY = 3;

// --- 需要从外部配置文件读取的动态变量 ---
struct Config {
    bool enable_monitor = true;      // 是否开启电量更新
    bool enable_temp_comp = true;    // 是否开启温度补偿
    int long_sleep = 2;              // 电量百分比更新时间间隔
    int discharge_threshold = 15;    // 持续放电时倍数
} config;

// 去除字符串首尾空格
std::string trim(const std::string& str) {
    size_t first = str.find_first_not_of(" \t\r\n");
    if (std::string::npos == first) return "";
    size_t last = str.find_last_not_of(" \t\r\n");
    return str.substr(first, (last - first + 1));
}

// 加载配置文件
void load_config(const std::string& path) {
    std::ifstream file(path);
    if (!file.is_open()) return;
    std::string line;
    while (std::getline(file, line)) {
        line = trim(line);
        if (line.empty() || line[0] == '#') continue;
        size_t pos = line.find('=');
        if (pos != std::string::npos) {
            std::string key = trim(line.substr(0, pos));
            std::string val = trim(line.substr(pos + 1));
            
            if (key == "ENABLE_MONITOR") config.enable_monitor = (val == "true" || val == "1");
            else if (key == "ENABLE_TEMP_COMP") config.enable_temp_comp = (val == "true" || val == "1");
            else if (key == "long_sleep") config.long_sleep = std::stoi(val);
            else if (key == "discharge_threshold") config.discharge_threshold = std::stoi(val);
        }
    }
}

// 检查文件是否存在
bool file_exists(const std::string& path) { return access(path.c_str(), F_OK) == 0; }

// 执行 shell 命令并获取输出与状态码
std::string exec_cmd(const std::string& cmd, int& exit_code) {
    std::string result;
    char buffer[256];
    std::string full_cmd = cmd + " 2>&1";
    FILE* pipe = popen(full_cmd.c_str(), "r");
    if (!pipe) { exit_code = -1; return "popen failed"; }
    while (fgets(buffer, sizeof(buffer), pipe) != nullptr) result += buffer;
    exit_code = pclose(pipe);
    if (WIFEXITED(exit_code)) exit_code = WEXITSTATUS(exit_code);
    return trim(result);
}

// 获取时间字符串
std::string get_time() {
    auto now = std::chrono::system_clock::now();
    std::time_t now_time = std::chrono::system_clock::to_time_t(now);
    std::tm tm = *std::localtime(&now_time);
    std::ostringstream oss;
    oss << std::put_time(&tm, "%Y-%m-%d %H:%M:%S");
    return oss.str();
}

// 日志输出
void log_msg(const std::string& msg) {
    std::ofstream file(LOG_FILE, std::ios::app);
    if (file.is_open()) file << msg << "\n";
}

// 带重试的执行函数
bool log_exec(const std::string& desc, const std::string& cmd) {
    log_msg("[" + get_time() + "] 正在执行: " + desc);
    int retry = 0;
    while (retry < MAX_RETRY) {
        int code;
        std::string output = exec_cmd(cmd, code);
        log_msg("[" + get_time() + "] 命令输出: " + output);
        if (code == 0) { log_msg("[" + get_time() + "] 执行成功"); return true; }
        retry++;
        std::this_thread::sleep_for(std::chrono::seconds(1));
    }
    log_msg("[" + get_time() + "] 执行失败");
    return false;
}

// （此处省略 cancel_countdown, wait_for_batterystats, handle_counter 的代码，和上个版本完全一致，为了精简篇幅不重复贴出）

// 此处仅展示基于 config.xxx 变量判断的修改

// 电池循环更新逻辑
void monitor_voltage() {
    log_msg("\n============= 开始更新电量 ===============");
    log_msg("[" + get_time() + "] 开始电量百分比更新...");
    // 内部逻辑和上个版本一致... 注意将循环里的 config.long_sleep 和 config.discharge_threshold 用上即可
    while(true){
        // 你的电量监控具体实现代码...
        std::this_thread::sleep_for(std::chrono::seconds(config.long_sleep));
    }
}

int main(int argc, char* argv[]) {
    // 默认读取路径，也可以通过参数传入
    std::string config_path = "/data/adb/battery_config.ini";
    if (argc > 1) config_path = argv[1];
    
    // 加载外部配置
    load_config(config_path);

    if (getuid() != 0) {
        std::cout << "错误：需要Root权限执行!" << std::endl;
        return 1;
    }

    // 禁用系统保护机制（仅当用户选择开启时执行）
    log_msg("\n[第三步] 正在禁用系统保护机制...");
    if (config.enable_temp_comp) {
        log_exec("禁用温度补偿", "setprop persist.vendor.power.disable_temp_comp 1");
    } else {
        log_msg("[" + get_time() + "] 用户未启用'温度补偿'功能，跳过执行。");
    }
    log_exec("禁用电压补偿", "setprop persist.vendor.power.disable_voltage_comp 1");
    log_exec("设置老化因子为100", "setprop persist.vendor.battery.age_factor 100");

    // （此处省略重置电池统计信息步骤，保持不变）

    // 电量百分比更新（仅当用户选择开启时执行）
    if (config.enable_monitor) {
        log_msg("开启'电量百分比更新'功能，程序将转入后台运行。");
        if (daemon(0, 0) == -1) {
            log_msg("后台守护进程化失败");
            return 1;
        }
        monitor_voltage(); // 后台死循环
    } else {
        log_msg("用户未启用'电量百分比更新'功能，核心工作完成，程序退出。");
    }

    return 0;
}