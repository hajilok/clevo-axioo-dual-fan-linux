/*
 ============================================================================
 Name        : clevo-indicator-dual.c
 Description : Ubuntu fan control indicator for Clevo laptops (DUAL FAN)

 Based on clevo-indicator by AqD <iiiaqd@gmail.com/m/jonas-diemer>
  -> https://github.com/SkyLandTW/clevo-indicator

 Modified to control BOTH fans (CPU fan #1 + GPU fan #2) on dual-fan
 Clevo models (e.g. P7xxDM, N1xxED, NH5x_7x, ND, NE, etc.) where the
 upstream version only spins up the left/CPU fan.

 EC protocol (verified against tuxedo-fan-control native/ec_access.cc):
   Write fan duty  -> cmd 0x99, fan-index (0x01=CPU, 0x02=GPU, 0x03=Fan3),
                      raw duty byte (0-255)
   Set fan to AUTO -> cmd 0x99, 0xff, fan-index
   Read CPU fan RPM -> EC register 0xD0 (hi) / 0xD1 (lo)
   Read GPU fan RPM -> EC register 0xD2 (hi) / 0xD3 (lo)
   Read CPU temp     -> EC register 0x07
   Read GPU temp     -> EC register 0xCD

 Build:
   make
   sudo chown root bin/clevo-indicator-dual
   sudo chmod u+s   bin/clevo-indicator-dual
 ============================================================================
 */

#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <math.h>
#include <signal.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/io.h>
#include <sys/mman.h>
#include <sys/prctl.h>
#include <sys/syscall.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <unistd.h>

#include <libappindicator/app-indicator.h>

#define NAME "clevo-indicator-dual"

#define EC_SC 0x66
#define EC_DATA 0x62

#define IBF 1
#define OBF 0
#define EC_SC_READ_CMD 0x80

/* EC registers can be read via EC_SC_READ_CMD or /sys/kernel/debug/ec/ec0/io:
 *
 * 1. modprobe ec_sys
 * 2. od -Ax -t x1 /sys/kernel/debug/ec/ec0/io
 */

#define EC_REG_SIZE 0x100
#define EC_REG_CPU_TEMP 0x07
#define EC_REG_GPU_TEMP 0xCD
#define EC_REG_FAN1_DUTY 0xCE        /* CPU fan duty (mirror of last written) */
#define EC_REG_FAN1_RPMS_HI 0xD0
#define EC_REG_FAN1_RPMS_LO 0xD1
#define EC_REG_FAN2_RPMS_HI 0xD2     /* GPU fan RPM (second fan) */
#define EC_REG_FAN2_RPMS_LO 0xD3

#define EC_CMD_FAN_DUTY 0x99
#define EC_FAN_INDEX_CPU 0x01
#define EC_FAN_INDEX_GPU 0x02
#define EC_FAN_INDEX_AUTO 0xff       /* followed by fan index => auto mode */

#define MAX_FAN_RPM 4400.0

/* Fan duty below this value is clamped because most Clevo fans stall below ~40%.
 * The EC write helper enforces a sane minimum. */
#define MIN_FAN_DUTY 40
#define MAX_FAN_DUTY 100

typedef enum {
    NA = 0, AUTO = 1, MANUAL = 2
} MenuItemType;

typedef enum {
    FAN_CPU = 1, FAN_GPU = 2
} FanIndex;

static void main_init_share(void);
static int main_ec_worker(void);
static void main_ui_worker(int argc, char** argv);
static void main_on_sigchld(int signum);
static void main_on_sigterm(int signum);
static int main_dump_fan(void);
static int main_test_fan(int duty_percentage, int fan_index);
static gboolean ui_update(gpointer user_data);
static void ui_command_set_fan(long fan_duty);
static void ui_command_quit(gchar* command);
static void ui_toggle_menuitems(int fan_duty);
static void ec_on_sigterm(int signum);
static int ec_init(void);
static int ec_auto_duty_adjust(void);
static int ec_query_cpu_temp(void);
static int ec_query_gpu_temp(void);
static int ec_query_fan1_duty(void);
static int ec_query_fan1_rpms(void);
static int ec_query_fan2_rpms(void);
static int ec_write_fan_duty(int duty_percentage, int fan_index);
static int ec_io_wait(const uint32_t port, const uint32_t flag,
        const char value);
static uint8_t ec_io_read(const uint32_t port);
static int ec_io_do(const uint32_t cmd, const uint32_t port,
        const uint8_t value);
static int calculate_fan_duty(int raw_duty);
static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low);
static int check_proc_instances(const char* proc_name);
static void get_time_string(char* buffer, size_t max, const char* format);
static void signal_term(__sighandler_t handler);

static AppIndicator* indicator = NULL;

struct {
    char label[256];
    GCallback callback;
    long option;
    MenuItemType type;
    GtkWidget* widget;

} static menuitems[] = {
        { "Set BOTH FANS to AUTO", G_CALLBACK(ui_command_set_fan), 0, AUTO, NULL },
        { "", NULL, 0L, NA, NULL },
        { "Set BOTH FANS to  40%", G_CALLBACK(ui_command_set_fan), 40, MANUAL, NULL },
        { "Set BOTH FANS to  50%", G_CALLBACK(ui_command_set_fan), 50, MANUAL, NULL },
        { "Set BOTH FANS to  60%", G_CALLBACK(ui_command_set_fan), 60, MANUAL, NULL },
        { "Set BOTH FANS to  70%", G_CALLBACK(ui_command_set_fan), 70, MANUAL, NULL },
        { "Set BOTH FANS to  80%", G_CALLBACK(ui_command_set_fan), 80, MANUAL, NULL },
        { "Set BOTH FANS to  90%", G_CALLBACK(ui_command_set_fan), 90, MANUAL, NULL },
        { "Set BOTH FANS to 100%", G_CALLBACK(ui_command_set_fan), 100, MANUAL, NULL },
        { "", NULL, 0L, NA, NULL },
        { "Quit", G_CALLBACK(ui_command_quit), 0L, NA, NULL }
};

static int menuitem_count = (sizeof(menuitems) / sizeof(menuitems[0]));

struct {
    volatile int exit;
    volatile int cpu_temp;
    volatile int gpu_temp;
    volatile int fan1_duty;
    volatile int fan1_rpms;
    volatile int fan2_duty;
    volatile int fan2_rpms;
    volatile int auto_duty;
    volatile int auto_duty_val;
    volatile int manual_next_fan_duty;
    volatile int manual_prev_fan_duty;
} static *share_info = NULL;

static pid_t parent_pid = 0;

int main(int argc, char* argv[]) {
    printf("Dual fan control utility for Clevo laptops (CPU + GPU)\n");
    if (check_proc_instances(NAME) > 1) {
        printf("Multiple running instances!\n");
        char* display = getenv("DISPLAY");
        if (display != NULL && strlen(display) > 0) {
            int desktop_uid = getuid();
            setuid(desktop_uid);
            //
            gtk_init(&argc, &argv);
            GtkWidget* dialog = gtk_message_dialog_new(NULL, 0,
                    GTK_MESSAGE_ERROR, GTK_BUTTONS_CLOSE,
                    "Multiple running instances of %s!", NAME);
            gtk_dialog_run(GTK_DIALOG(dialog));
            gtk_widget_destroy(dialog);
        }
        return EXIT_FAILURE;
    }
    if (ec_init() != EXIT_SUCCESS) {
        printf("unable to control EC: %s\n", strerror(errno));
        return EXIT_FAILURE;
    }
    if (argc <= 1) {
        char* display = getenv("DISPLAY");
        if (display == NULL || strlen(display) == 0) {
            return main_dump_fan();
        } else {
            parent_pid = getpid();
            main_init_share();
            signal(SIGCHLD, &main_on_sigchld);
            signal_term(&main_on_sigterm);
            pid_t worker_pid = fork();
            if (worker_pid == 0) {
                signal(SIGCHLD, SIG_DFL);
                signal_term(&ec_on_sigterm);
                return main_ec_worker();
            } else if (worker_pid > 0) {
                main_ui_worker(argc, argv);
                share_info->exit = 1;
                waitpid(worker_pid, NULL, 0);
            } else {
                printf("unable to create worker: %s\n", strerror(errno));
                return EXIT_FAILURE;
            }
        }
    } else {
        if (argv[1][0] == '-') {
            printf(
                    "\n\
Usage: clevo-indicator-dual [fan-duty-percentage] [fan-index]\n\
\n\
Dump/Control fan duty on dual-fan Clevo laptops. Display indicator by default.\n\
\n\
Arguments:\n\
  [fan-duty-percentage]   Target fan duty in percentage, from %d to %d.\n\
                          Applied to BOTH fans if [fan-index] is omitted.\n\
  [fan-index]             1 = CPU fan only, 2 = GPU fan only.\n\
                          (default: both fans = 1 then 2)\n\
  -?                      Display this help and exit\n\
\n\
Without arguments this program attempts to display an indicator in the\n\
Ubuntu tray area for fan information display and control of BOTH fans.\n\
The indicator requires this program to have setuid=root flag but run from\n\
the desktop user, because a root user is not allowed to display a desktop\n\
indicator while a non-root user is not allowed to control Clevo EC\n\
(Embedded Controller responsible for the fans). Fix permissions:\n\
    sudo chown root clevo-indicator-dual\n\
    sudo chmod u+s   clevo-indicator-dual\n\
\n\
This is a DUAL-FAN fork: it writes fan duty to EC command 0x99 for BOTH\n\
fan index 0x01 (CPU) and fan index 0x02 (GPU), so both fans spin up.\n\
\n\
DO NOT MANIPULATE OR QUERY EC I/O PORTS WHILE THIS PROGRAM IS RUNNING.\n\
\n",
                    MIN_FAN_DUTY, MAX_FAN_DUTY);
            return main_dump_fan();
        } else {
            int val = atoi(argv[1]);
            if (val < MIN_FAN_DUTY || val > MAX_FAN_DUTY) {
                printf("invalid fan duty %d! (valid %d-%d)\n", val,
                        MIN_FAN_DUTY, MAX_FAN_DUTY);
                return EXIT_FAILURE;
            }
            int fan_index = 0; /* 0 = both */
            if (argc >= 3) {
                fan_index = atoi(argv[2]);
                if (fan_index != 1 && fan_index != 2) {
                    printf("invalid fan index %d! (1=CPU, 2=GPU)\n",
                            fan_index);
                    return EXIT_FAILURE;
                }
            }
            return main_test_fan(val, fan_index);
        }
    }
    return EXIT_SUCCESS;
}

static void main_init_share(void) {
    void* shm = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_ANON | MAP_SHARED,
            -1, 0);
    share_info = shm;
    share_info->exit = 0;
    share_info->cpu_temp = 0;
    share_info->gpu_temp = 0;
    share_info->fan1_duty = 0;
    share_info->fan1_rpms = 0;
    share_info->fan2_duty = 0;
    share_info->fan2_rpms = 0;
    share_info->auto_duty = 1;
    share_info->auto_duty_val = 0;
    share_info->manual_next_fan_duty = 0;
    share_info->manual_prev_fan_duty = 0;
}

static int main_ec_worker(void) {
    setuid(0);
    system("modprobe ec_sys");
    while (share_info->exit == 0) {
        // check parent
        if (parent_pid != 0 && kill(parent_pid, 0) == -1) {
            printf("worker on parent death\n");
            break;
        }
        // write EC (manual mode) - apply to BOTH fans
        int new_fan_duty = share_info->manual_next_fan_duty;
        if (new_fan_duty != 0
                && new_fan_duty != share_info->manual_prev_fan_duty) {
            ec_write_fan_duty(new_fan_duty, FAN_CPU);
            ec_write_fan_duty(new_fan_duty, FAN_GPU);
            share_info->manual_prev_fan_duty = new_fan_duty;
        }
        // read EC
        int io_fd = open("/sys/kernel/debug/ec/ec0/io", O_RDONLY, 0);
        if (io_fd < 0) {
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            exit(EXIT_FAILURE);
        }
        unsigned char buf[EC_REG_SIZE];
        ssize_t len = read(io_fd, buf, EC_REG_SIZE);
        switch (len) {
        case -1:
            printf("unable to read EC from sysfs: %s\n", strerror(errno));
            break;
        case 0x100:
            share_info->cpu_temp = buf[EC_REG_CPU_TEMP];
            share_info->gpu_temp = buf[EC_REG_GPU_TEMP];
            share_info->fan1_duty = calculate_fan_duty(buf[EC_REG_FAN1_DUTY]);
            share_info->fan1_rpms = calculate_fan_rpms(
                    buf[EC_REG_FAN1_RPMS_HI], buf[EC_REG_FAN1_RPMS_LO]);
            share_info->fan2_rpms = calculate_fan_rpms(
                    buf[EC_REG_FAN2_RPMS_HI], buf[EC_REG_FAN2_RPMS_LO]);
            /* GPU fan has no separate duty register mirror on most ECs,
             * assume it tracks the CPU fan duty when we drive both. */
            share_info->fan2_duty = share_info->fan1_duty;
            /*
             printf("cpu=%dC gpu=%dC duty=%d%% rpm1=%d rpm2=%d\n",
                    share_info->cpu_temp, share_info->gpu_temp,
                    share_info->fan1_duty, share_info->fan1_rpms,
                    share_info->fan2_rpms);
             */
            break;
        default:
            printf("wrong EC size from sysfs: %ld\n", len);
        }
        close(io_fd);
        // auto EC - apply to BOTH fans
        if (share_info->auto_duty == 1) {
            int next_duty = ec_auto_duty_adjust();
            if (next_duty != 0 && next_duty != share_info->auto_duty_val) {
                char s_time[256];
                get_time_string(s_time, 256, "%m/%d %H:%M:%S");
                printf(
                        "%s CPU=%dC, GPU=%dC, auto fan duty to %d%% (BOTH fans)\n",
                        s_time, share_info->cpu_temp, share_info->gpu_temp,
                        next_duty);
                ec_write_fan_duty(next_duty, FAN_CPU);
                ec_write_fan_duty(next_duty, FAN_GPU);
                share_info->auto_duty_val = next_duty;
            }
        }
        //
        usleep(200 * 1000);
    }
    printf("worker quit\n");
    return EXIT_SUCCESS;
}

static void main_ui_worker(int argc, char** argv) {
    printf("Indicator (dual fan)...\n");
    int desktop_uid = getuid();
    setuid(desktop_uid);
    //
    gtk_init(&argc, &argv);
    //
    GtkWidget* indicator_menu = gtk_menu_new();
    for (int i = 0; i < menuitem_count; i++) {
        GtkWidget* item;
        if (strlen(menuitems[i].label) == 0) {
            item = gtk_separator_menu_item_new();
        } else {
            item = gtk_menu_item_new_with_label(menuitems[i].label);
            g_signal_connect_swapped(item, "activate",
                    G_CALLBACK(menuitems[i].callback),
                    (void* ) menuitems[i].option);
        }
        gtk_menu_shell_append(GTK_MENU_SHELL(indicator_menu), item);
        menuitems[i].widget = item;
    }
    gtk_widget_show_all(indicator_menu);
    //
    indicator = app_indicator_new(NAME, "brasero",
            APP_INDICATOR_CATEGORY_HARDWARE);
    g_assert(IS_APP_INDICATOR(indicator));
    app_indicator_set_label(indicator, "Init..", "XX");
    app_indicator_set_status(indicator, APP_INDICATOR_STATUS_ATTENTION);
    app_indicator_set_ordering_index(indicator, -2);
    app_indicator_set_title(indicator, "Clevo Dual");
    app_indicator_set_menu(indicator, GTK_MENU(indicator_menu));
    g_timeout_add(500, &ui_update, NULL);
    ui_toggle_menuitems(share_info->fan1_duty);
    gtk_main();
    printf("main on UI quit\n");
}

static void main_on_sigchld(int signum) {
    printf("main on worker quit signal\n");
    exit(EXIT_SUCCESS);
}

static void main_on_sigterm(int signum) {
    printf("main on signal: %s\n", strsignal(signum));
    if (share_info != NULL)
        share_info->exit = 1;
    exit(EXIT_SUCCESS);
}

static int main_dump_fan(void) {
    printf("Dump fan information (dual)\n");
    printf("  CPU FAN Duty: %d%%\n", ec_query_fan1_duty());
    printf("  CPU FAN RPMs: %d RPM\n", ec_query_fan1_rpms());
    printf("  GPU FAN RPMs: %d RPM\n", ec_query_fan2_rpms());
    printf("  CPU Temp:     %dC\n", ec_query_cpu_temp());
    printf("  GPU Temp:     %dC%s\n", ec_query_gpu_temp(),
            ec_query_gpu_temp() == 0 ? "  (GPU idle/Optimus asleep)" : "");
    return EXIT_SUCCESS;
}

static int main_test_fan(int duty_percentage, int fan_index) {
    printf("Change fan duty to %d%% (index=%s)\n", duty_percentage,
            fan_index == 0 ? "BOTH" : (fan_index == 1 ? "CPU" : "GPU"));
    if (fan_index == 0 || fan_index == 1)
        ec_write_fan_duty(duty_percentage, FAN_CPU);
    if (fan_index == 0 || fan_index == 2)
        ec_write_fan_duty(duty_percentage, FAN_GPU);
    printf("\n");
    main_dump_fan();
    return EXIT_SUCCESS;
}

static gboolean ui_update(gpointer user_data) {
    /* Label: CPUtemp / GPUtemp and the two fan RPMs so you can SEE that
     * both fans are spinning. */
    char label[256];
    sprintf(label, "%dC %dC", share_info->cpu_temp, share_info->gpu_temp);
    app_indicator_set_label(indicator, label, "XXXXXX");

    /* Tooltip: detailed dual-fan status */
    char title[256];
    snprintf(title, sizeof(title),
            "Clevo Dual Fan | CPU %dC %dRPM | GPU %dC %dRPM",
            share_info->cpu_temp, share_info->fan1_rpms, share_info->gpu_temp,
            share_info->fan2_rpms);
    app_indicator_set_title(indicator, title);

    /* Icon reflects the faster of the two fans. */
    char icon_name[256];
    int max_rpms = MAX(share_info->fan1_rpms, share_info->fan2_rpms);
    double load = ((double) max_rpms) / MAX_FAN_RPM * 100.0;
    double load_r = round(load / 5.0) * 5.0;
    if (load_r > 100.0)
        load_r = 100.0;
    sprintf(icon_name, "brasero-disc-%02d", (int) load_r);
    app_indicator_set_icon(indicator, icon_name);
    return G_SOURCE_CONTINUE;
}

static void ui_command_set_fan(long fan_duty) {
    int fan_duty_val = (int) fan_duty;
    if (fan_duty_val == 0) {
        printf("clicked on fan duty auto (both fans)\n");
        share_info->auto_duty = 1;
        share_info->auto_duty_val = 0;
        share_info->manual_next_fan_duty = 0;
    } else {
        printf("clicked on fan duty: %d (both fans)\n", fan_duty_val);
        share_info->auto_duty = 0;
        share_info->auto_duty_val = 0;
        share_info->manual_next_fan_duty = fan_duty_val;
    }
    ui_toggle_menuitems(fan_duty_val);
}

static void ui_command_quit(gchar* command) {
    printf("clicked on quit\n");
    gtk_main_quit();
}

static void ui_toggle_menuitems(int fan_duty) {
    for (int i = 0; i < menuitem_count; i++) {
        if (menuitems[i].widget == NULL)
            continue;
        if (fan_duty == 0)
            gtk_widget_set_sensitive(menuitems[i].widget,
                    menuitems[i].type != AUTO);
        else
            gtk_widget_set_sensitive(menuitems[i].widget,
                    menuitems[i].type != MANUAL
                            || (int) menuitems[i].option != fan_duty);
    }
}

static int ec_init(void) {
    if (ioperm(EC_DATA, 1, 1) != 0)
        return EXIT_FAILURE;
    if (ioperm(EC_SC, 1, 1) != 0)
        return EXIT_FAILURE;
    return EXIT_SUCCESS;
}

static void ec_on_sigterm(int signum) {
    printf("ec on signal: %s\n", strsignal(signum));
    if (share_info != NULL)
        share_info->exit = 1;
}

static int ec_auto_duty_adjust(void) {
    int temp = MAX(share_info->cpu_temp, share_info->gpu_temp);
    int duty = share_info->fan1_duty;
    //
    if (temp >= 80 && duty < 100)
        return 100;
    if (temp >= 70 && duty < 90)
        return 90;
    if (temp >= 60 && duty < 80)
        return 80;
    if (temp >= 50 && duty < 70)
        return 70;
    if (temp >= 40 && duty < 60)
        return 60;
    if (temp >= 30 && duty < 50)
        return 50;
    if (temp >= 20 && duty < 40)
        return 40;
    //
    if (temp <= 15 && duty > 40)
        return 40;
    if (temp <= 25 && duty > 50)
        return 50;
    if (temp <= 35 && duty > 60)
        return 60;
    if (temp <= 45 && duty > 70)
        return 70;
    if (temp <= 55 && duty > 80)
        return 80;
    if (temp <= 65 && duty > 90)
        return 90;
    //
    return 0;
}

static int ec_query_cpu_temp(void) {
    return ec_io_read(EC_REG_CPU_TEMP);
}

static int ec_query_gpu_temp(void) {
    return ec_io_read(EC_REG_GPU_TEMP);
}

static int ec_query_fan1_rpms(void) {
    int raw_rpm_hi = ec_io_read(EC_REG_FAN1_RPMS_HI);
    int raw_rpm_lo = ec_io_read(EC_REG_FAN1_RPMS_LO);
    return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
}

static int ec_query_fan1_duty(void) {
    int raw_duty = ec_io_read(EC_REG_FAN1_DUTY);
    return calculate_fan_duty(raw_duty);
}

static int ec_query_fan2_rpms(void) {
    int raw_rpm_hi = ec_io_read(EC_REG_FAN2_RPMS_HI);
    int raw_rpm_lo = ec_io_read(EC_REG_FAN2_RPMS_LO);
    return calculate_fan_rpms(raw_rpm_hi, raw_rpm_lo);
}

/* Write fan duty to a specific fan.
 * fan_index: FAN_CPU (0x01) or FAN_GPU (0x02).
 * This is the KEY difference vs upstream clevo-indicator, which only ever
 * wrote index 0x01. We now also write 0x02 so the second fan spins up. */
static int ec_write_fan_duty(int duty_percentage, int fan_index) {
    if (duty_percentage < MIN_FAN_DUTY || duty_percentage > MAX_FAN_DUTY) {
        printf("Wrong fan duty to write: %d (valid %d-%d)\n", duty_percentage,
                MIN_FAN_DUTY, MAX_FAN_DUTY);
        return EXIT_FAILURE;
    }
    double v_d = ((double) duty_percentage) / 100.0 * 255.0;
    int v_i = (int) v_d;
    uint32_t idx = (fan_index == FAN_GPU) ? 0x02 : 0x01;
    return ec_io_do(EC_CMD_FAN_DUTY, idx, v_i);
}

static int ec_io_wait(const uint32_t port, const uint32_t flag,
        const char value) {
    uint8_t data = inb(port);
    int i = 0;
    while ((((data >> flag) & 0x1) != value) && (i++ < 100)) {
        usleep(1000);
        data = inb(port);
    }
    if (i >= 100) {
        printf("wait_ec error on port 0x%x, data=0x%x, flag=0x%x, value=0x%x\n",
                port, data, flag, value);
        return EXIT_FAILURE;
    }
    return EXIT_SUCCESS;
}

static uint8_t ec_io_read(const uint32_t port) {
    ec_io_wait(EC_SC, IBF, 0);
    outb(EC_SC_READ_CMD, EC_SC);

    ec_io_wait(EC_SC, IBF, 0);
    outb(port, EC_DATA);

    //wait_ec(EC_SC, EC_SC_IBF_FREE);
    ec_io_wait(EC_SC, OBF, 1);
    uint8_t value = inb(EC_DATA);

    return value;
}

static int ec_io_do(const uint32_t cmd, const uint32_t port,
        const uint8_t value) {
    ec_io_wait(EC_SC, IBF, 0);
    outb(cmd, EC_SC);

    ec_io_wait(EC_SC, IBF, 0);
    outb(port, EC_DATA);

    ec_io_wait(EC_SC, IBF, 0);
    outb(value, EC_DATA);

    return ec_io_wait(EC_SC, IBF, 0);
}

static int calculate_fan_duty(int raw_duty) {
    return (int) ((double) raw_duty / 255.0 * 100.0);
}

static int calculate_fan_rpms(int raw_rpm_high, int raw_rpm_low) {
    int raw_rpm = (raw_rpm_high << 8) + raw_rpm_low;
    return raw_rpm > 0 ? (2156220 / raw_rpm) : 0;
}

static int check_proc_instances(const char* proc_name) {
    int proc_name_len = strlen(proc_name);
    pid_t this_pid = getpid();
    DIR* dir;
    if (!(dir = opendir("/proc"))) {
        perror("can't open /proc");
        return -1;
    }
    int instance_count = 0;
    struct dirent* ent;
    while ((ent = readdir(dir)) != NULL) {
        char* endptr;
        long lpid = strtol(ent->d_name, &endptr, 10);
        if (*endptr != '\0')
            continue;
        if (lpid == this_pid)
            continue;
        char buf[512];
        snprintf(buf, sizeof(buf), "/proc/%ld/comm", lpid);
        FILE* fp = fopen(buf, "r");
        if (fp) {
            if (fgets(buf, sizeof(buf), fp) != NULL) {
                if ((buf[proc_name_len] == '\n' || buf[proc_name_len] == '\0')
                        && strncmp(buf, proc_name, proc_name_len) == 0) {
                    fprintf(stderr, "Process: %ld\n", lpid);
                    instance_count += 1;
                }
            }
            fclose(fp);
        }
    }
    closedir(dir);
    return instance_count;
}

static void get_time_string(char* buffer, size_t max, const char* format) {
    time_t timer;
    struct tm tm_info;
    time(&timer);
    localtime_r(&timer, &tm_info);
    strftime(buffer, max, format, &tm_info);
}

static void signal_term(__sighandler_t handler) {
    signal(SIGHUP, handler);
    signal(SIGINT, handler);
    signal(SIGQUIT, handler);
    signal(SIGPIPE, handler);
    signal(SIGALRM, handler);
    signal(SIGTERM, handler);
    signal(SIGUSR1, handler);
    signal(SIGUSR2, handler);
}
