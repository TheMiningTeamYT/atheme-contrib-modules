extern "C" {
    #include "atheme-compat.h"
}

#include <vector>
#include <map>
#include <ctime>
#include <string>
#include <stdexcept>

/* A single message sent in a channel. */
struct Message {
    std::time_t sent;
    std::string nick;
    std::string content;
};

/* 
 * All messages sent in a channel
 * and the last time any users joined said channel.
 */
struct Channel {
    /* Nick : Time last seen */
    std::map<std::string, std::time_t> users;
    std::vector<Message> log;
};

static service_t *backlog = NULL;
static std::map<std::string, Channel> channels;

static void saw(const char* nick, const char* channel) {
    std::time_t now = std::time(nullptr);
    try {
        channels.at(channel).users.at(nick) = now;
    } catch (const std::out_of_range& e) {
        channels.at(channel).users.insert({nick, now});
    }
}

static void insert_message(const char* channel, const char* nick, const char* content) {
    std::time_t now = std::time(nullptr);
    Message message = {now, nick, content};
    try {
        channels.at(channel).log.push_back(message);
    } catch (const std::out_of_range& e) {
        /* 
         * If the channel this message was sent in is not already being tracked,
         * insert it into the list,
         * containing the message sent, and the user who sent it.
         * (We can assume the user is online if they're sending a message.)
         */
        channels.insert({channel, {{{nick, now}}, {message}} });
    }
}

static void send_backlog(const char* nick, const char* channel) {
    std::time_t since;
    /* 
     * Is this pointer safe?
     * I'm worried about a potential race condition. -Loganius
     */
    std::vector<Message> *log;
    std::time_t now = std::time(nullptr);
    std::tm today = *std::gmtime(&now);
    bool sentTimestamp = false;

    try {
        log = &channels.at(channel).log;
    } catch (const std::out_of_range& e) {
        return;
    }
    
    try {
        since = channels.at(channel).users.at(nick);
    } catch (const std::out_of_range& e) {
        since = 0;
    }

    for (size_t i = log->size(); i > 0; i--) {
        if (i <= 100) {
            Message &message = log->at(log->size() - i);
            if (message.sent > since) {
                /* Send a timestamp message to let the user know when the backlog begins. */
                if (!sentTimestamp) {
                    std::tm *sent = std::gmtime(&message.sent);
                    char timestamp[512];
                    if (today.tm_yday != sent->tm_yday || today.tm_year != sent->tm_year) {
                        std::strftime(timestamp, sizeof(timestamp), "%r %Z", sent);
                    } else {
                        std::strftime(timestamp, sizeof(timestamp), "%r %B %d, %Y %Z", sent);
                    }
                    msg("BacklogServ", nick, "Backlog for %s since %s.", channel, timestamp);
                    sentTimestamp = true;
                }
                msg(message.nick.c_str(), nick, "%s", message.content.c_str());
            }
        }
    }
}

static void on_message(hook_cmessage_data_t *msg) {
    insert_message(msg->c->name, msg->u->nick, msg->msg);
    saw(msg->u->nick, msg->c->name);
}

static void on_join(hook_channel_joinpart_t *join) {
    send_backlog(join->cu->user->nick, join->cu->chan->name);
    saw(join->cu->user->nick, join->cu->chan->name);
}

static void on_part(hook_channel_joinpart_t *part) {
    saw(part->cu->user->nick, part->cu->chan->name);
}

static void mod_init(module_t *m) {
    hook_add_event("channel_message");
    hook_add_event("channel_join");
    hook_add_event("channel_part");
	hook_add_channel_message(on_message);
    hook_add_channel_join(on_join);
    hook_add_channel_part(on_part);
}

static void mod_deinit(const module_unload_intent_t intent) {
    hook_del_channel_message(on_message);
    hook_del_channel_join(on_join);
    hook_del_channel_part(on_part);
	service_delete(backlog);
}

VENDOR_DECLARE_MODULE_V1("contrib/backlog", MODULE_UNLOAD_CAPABILITY_OK, CONTRIB_VENDOR_LOGANIUS)