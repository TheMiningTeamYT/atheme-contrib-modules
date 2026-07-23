/*
 * SPDX-License-Identifier: ISC
 * SPDX-URL: https://spdx.org/licenses/ISC.html
 *
 * Copyright (C) 2026 Logan C. (loganisamazing@outlook.com)
 *
 * A bot which records all the messages in one or more channels
 * and replays them to users when they join.
 * 
 * TODO:
 * Allow admins to set a custom server-wide backlog length.
 * Use mowgli lists like the rest of Atheme instead of maps & vectors.
 */

extern "C" {
    #include "atheme-compat.h"
}

#include <vector>
#include <unordered_map>
#include <ctime>
#include <cstdlib>
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
    std::unordered_map<std::string, std::time_t> users;
    std::vector<Message> log;
};

struct {
    service_t *me;
} backlog;

static std::unordered_map<std::string, Channel> channels;

/* The default backlog length. */
static unsigned int backlogLength = 100;

/* Allow BacklogServ to record a channel's backlog. */
#define BACKLOG_ENABLE_MD "backlog:enable"

/* Allow users to stop BacklogServ from speaking to them. */
#define BACKLOG_DEAFEN_MD "backlog:mute"

/* Allow users to stop BacklogServ from recording their messages. */
#define BACKLOG_MUTE_MD "backlog:deafen"

/* How many messages BacklogServ should send when the user joins a channel. */
#define BACKLOG_LENGTH_MD "backlog:length"

/* Allow BacklogServ to automatically send the backlog to newly arrived users. */
#define BACKLOG_AUTOSEND (0x1)

static void saw(const char* nick, const char* channel) {
    std::time_t now = std::time(nullptr);
    try {
        Channel &channelStruct = channels.at(channel);
        try {
            channelStruct.users.at(nick) = now;
        } catch (const std::out_of_range& e) {
            channelStruct.users.insert({nick, now});
        }
    } catch (const std::out_of_range& e) {
        channels.insert({channel, {{{nick, now}}, {}}});
        join(channel, backlog.me->nick);
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
        join(channel, backlog.me->nick);
    }
}

static void send_backlog(user_t *user, const char* channel, unsigned int length, bool all) {
    std::time_t since = 0;
    /* 
     * Is this pointer safe?
     * I'm worried about a potential race condition. -Loganius
     */
    std::vector<Message> *log;
    std::time_t now = std::time(nullptr);
    std::tm today = *std::gmtime(&now);
    bool sentTimestamp = false;
    char buf[1024];

    try {
        log = &channels.at(channel).log;
    } catch (const std::out_of_range& e) {
        return;
    }
    
    if (!all) {
        try {
            since = channels.at(channel).users.at(user->nick);
        } catch (const std::out_of_range& e) {}
    }

    for (size_t i = log->size(); i > 0; i--) {
        if (i <= length) {
            Message &message = log->at(log->size() - i);
            if (message.sent > since) {
                /* Send a timestamp message to let the user know when the backlog begins. */
                if (!sentTimestamp) {
                    std::tm *sent = std::gmtime(&message.sent);
                    char timestamp[256];
                    if (today.tm_yday != sent->tm_yday || today.tm_year != sent->tm_year) {
                        std::strftime(timestamp, sizeof(timestamp), "%r %B %d, %Y %Z", sent);
                    } else {
                        std::strftime(timestamp, sizeof(timestamp), "%r %Z", sent);
                    }
                    snprintf(buf, sizeof(buf), "Backlog for %s since %s.", channel, timestamp);
                    notice_user_sts(backlog.me->me, user, buf);
                    sentTimestamp = true;
                }
                snprintf(buf, sizeof(buf), "[%s] \02%s\02: %s", channel, message.nick.c_str(), message.content.c_str());
                notice_user_sts(backlog.me->me, user, buf);
            }
        }
    }
}

/*
 * Utility function for command handlers which checks that the channel exists 
 * and that the user is OP-ed in said channel.
*/
static mychan_t *chan_cmd_check_permissions(struct sourceinfo *si, int parc, char *parv[], const char *cmdName) {
    /* Based on the code for the REGISTER command of ChanServ. */
    mychan_t *mc;
    /* FYI this code in ChanServ seems like it is potentially OOB. -Loganius */
    char *name;

    if (parc != 1 || (name = parv[0]) == nullptr) {
        command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, cmdName);
		command_fail(si, fault_needmoreparams, _("Syntax: %s %s <#channel>"), backlog.me->nick, cmdName);
		return nullptr;
    }

    /* Make sure the name specified is actually a channel name. */
    if (name[0] != '#') {
        command_fail(si, fault_badparams, STR_INVALID_PARAMS, cmdName);
		command_fail(si, fault_badparams, _("Syntax: %s %s <#channel>"), backlog.me->nick, cmdName);
		return nullptr;
    }

    /* 
     * Make sure the channel exists and is registered so we can save metadata to it. 
     * Is this necessary? -Loganius
     */
    if (!(mc = mychan_find(name))) {
        command_fail(si, fault_badparams, STR_IS_NOT_REGISTERED, name);
        return nullptr;
    }

    /* Make sure the user is an operator in the channel. */
    if (!chanacs_source_has_flag(mc, si, CA_OP) && !chanacs_source_has_flag(mc, si, CA_AUTOOP)) {
        command_fail(si, fault_noprivs, STR_NOT_AUTHORIZED);
        return nullptr;
    }

    return mc;
}

static void on_message(hook_cmessage_data_t *msg) {
    /* Code stolen from babbler. */
    if (msg != NULL && msg->msg != NULL) {
        mychan_t *mc = msg->c->mychan;
        void* user = msg->u->myuser ? (void*)msg->u->myuser : msg->u;
        
        /* 
         * Only listen in channels we've been attached to
         * and users which haven't asked to be ignored.
        */
        if (!mc)
            return;
        if (!metadata_find(mc, BACKLOG_ENABLE_MD))
            return;
        if (metadata_find(user, BACKLOG_MUTE_MD))
            return;

        insert_message(msg->c->name, msg->u->nick, msg->msg);
        slog(LG_DEBUG, "%s: Saw message from %s.", backlog.me->nick, msg->u->nick);
        /* Should be unecessary if we log them when they join and when they leave. -Loganius */
        /* saw(msg->u->nick, msg->c->name); */
    }
}

static void on_join(hook_channel_joinpart_t *join) {
    mychan_t *mc = join->cu->chan->mychan;
    void* user = join->cu->user->myuser ? (void*)join->cu->user->myuser : join->cu->user;
    metadata_t *md;
    unsigned int length = backlogLength;

    /* 
     * Only send the backlog for channels which we have been attached to
     * and for users which haven't asked to not receive the backlog.
     */
    if (!mc)
        return;
    if (!(md = metadata_find(mc, BACKLOG_ENABLE_MD)) || !md->value)
        return;
    if (!(std::atoi(md->value) & BACKLOG_AUTOSEND) || metadata_find(user, BACKLOG_DEAFEN_MD))
        return;

    /* Check if the user has set a default backlog length. */
    if ((md = metadata_find(user, BACKLOG_LENGTH_MD)) && md->value) {
        /* If so, use it. */
        length = std::atoi(md->value);
    }

    send_backlog(join->cu->user, join->cu->chan->name, length, false);

    /* Only log the appearences of users which haven't asked to be ignored. */
    if (!metadata_find(user, BACKLOG_MUTE_MD)) {
        saw(join->cu->user->nick, join->cu->chan->name);
    }
}

static void on_part(hook_channel_joinpart_t *part) {
    mychan_t *mc = part->cu->chan->mychan;
    void* user = part->cu->user->myuser ? (void*)part->cu->user->myuser : part->cu->user;

    /* 
     * Only log users which haven't asked to be ignored 
     * in channels which we've been asked to attach to.
     */
    if (!mc)
        return;
    if (!metadata_find(mc, BACKLOG_ENABLE_MD))
        return;
    if (metadata_find(user, BACKLOG_MUTE_MD))
        return;

    saw(part->cu->user->nick, part->cu->chan->name);
}

static void on_del(user_t *user) {
    mowgli_node_t *n;
    void* userObj = user->myuser ? (void*)user->myuser : user;

    /* Only log users which haven't asked to be ignored.*/
    if (metadata_find(userObj, BACKLOG_MUTE_MD))
        return;

    MOWGLI_LIST_FOREACH(n, user->channels.head) {
        /*  Only log users in channels which we've been asked to attach to. */
        chanuser_t* cu = (chanuser_t*)n->data;
        mychan_t *mc = cu->chan->mychan;
        if (!mc || !metadata_find(mc, BACKLOG_ENABLE_MD))
            continue;
        saw(user->nick, cu->chan->name);
    }
}

/* Add BacklogServ to a channel */
static void cmd_join(struct sourceinfo *si, int parc, char *parv[]) {
    mychan_t *mc;
    char *name;

    if (!(mc = chan_cmd_check_permissions(si, parc, parv, "JOIN"))) {
        return;
    }

    name = parv[0];

    /* Make sure we aren't already in the channel. */
    if (metadata_find(mc, BACKLOG_ENABLE_MD)) {
        command_fail(si, fault_alreadyexists, _("%s is already recording %s."), backlog.me->nick, mc->chan->name);
        return;
    }

    /* Join the channel and allow BacklogServ to operate there. */
    if (!metadata_add(mc, BACKLOG_ENABLE_MD, std::to_string(BACKLOG_AUTOSEND).c_str())) {
        command_fail(si, fault_internalerror, _("An internal error occured in %s at line %d."), __FILE__, __LINE__);
        return;
    }

    join(mc->chan->name, backlog.me->nick);
    command_success_nodata(si, _("%s has successfully joined %s"), backlog.me->nick, mc->chan->name);
}

/* Remove BacklogServ from a channel. */
static void cmd_leave(struct sourceinfo *si, int parc, char *parv[]) {
    mychan_t *mc;

    if (!(mc = chan_cmd_check_permissions(si, parc, parv, "LEAVE"))) {
        return;
    }

    char *name = parv[0];

    /* Make sure we are currently in the channel. */
    if (!metadata_find(mc, BACKLOG_ENABLE_MD)) {
        command_fail(si, fault_alreadyexists, _("%s is not currently in %s."), backlog.me->nick, name);
        return;
    }

    /* Remove BacklogServ from the channel and delete the log for it. */
    metadata_delete(mc, BACKLOG_ENABLE_MD);
    channels.erase(name);

    part(name, backlog.me->nick);
    command_success_nodata(si, _("%s has successfully left %s."), backlog.me->nick, name);
}

/* Stop BacklogServ from sending the backlog automatically. */
static void cmd_silence(struct sourceinfo *si, int parc, char *parv[]) {
    mychan_t *mc;
    metadata_t *md;
    int mode;
    char *name;

    if (!(mc = chan_cmd_check_permissions(si, parc, parv, "SILENCE"))) {
        return;
    }

    name = parv[0];

    /* Make sure we are currently in the channel. */
    if (!(md = metadata_find(mc, BACKLOG_ENABLE_MD))) {
        command_fail(si, fault_alreadyexists, _("%s is not currently in %s."), backlog.me->nick, name);
        return;
    }

    /* If this happens, things are getting bad. -Loganius */
    if (!md->value) {
        command_fail(si, fault_internalerror, _("An internal error occured in %s at line %d."), __FILE__, __LINE__);
        return;
    }

    mode = std::atoi(md->value);

    /* Make sure BacklogServ isn't already silenced. */
    if (!(mode & BACKLOG_AUTOSEND)) {
        command_fail(si, fault_alreadyexists, _("%s is already silenced in %s."), backlog.me->nick, name);
        return;
    }

    /* Silence BacklogServ */
    if (!metadata_add(mc, BACKLOG_ENABLE_MD, std::to_string(mode & ~BACKLOG_AUTOSEND).c_str())) {
        command_fail(si, fault_internalerror, _("An internal error occured in %s at line %d."), __FILE__, __LINE__);
        return;
    }

    command_success_nodata(si, _("%s was successfully silenced in %s."), backlog.me->nick, name);
}

/* Allow BacklogServ to send the backlog automatically. */
static void cmd_unsilence(struct sourceinfo *si, int parc, char *parv[]) {
    mychan_t *mc;
    metadata_t *md;
    int mode;
    char *name;

    if (!(mc = chan_cmd_check_permissions(si, parc, parv, "UNSILENCE"))) {
        return;
    }

    name = parv[0];

    /* Make sure we are currently in the channel. */
    if (!(md = metadata_find(mc, BACKLOG_ENABLE_MD))) {
        command_fail(si, fault_alreadyexists, _("%s is not currently in %s."), backlog.me->nick, name);
        return;
    }

    /* If this happens, things are getting bad. -Loganius */
    if (!md->value) {
        command_fail(si, fault_internalerror, _("An internal error occured in %s at line %d."), __FILE__, __LINE__);
        return;
    }

    mode = std::atoi(md->value);

    /* Make sure BacklogServ isn't already unsilenced. */
    if (mode & BACKLOG_AUTOSEND) {
        command_fail(si, fault_alreadyexists, _("%s is already allowed to send the backlog in %s."), backlog.me->nick, name);
        return;
    }

    /* Unsilence BacklogServ */
    if (!metadata_add(mc, BACKLOG_ENABLE_MD, std::to_string(mode | BACKLOG_AUTOSEND).c_str())) {
        command_fail(si, fault_internalerror, _("An internal error occured in %s at line %d."), __FILE__, __LINE__);
        return;
    }

    command_success_nodata(si, _("%s was successfully unsilenced in %s."), backlog.me->nick, name);
}

/* Stop BacklogServ from recording users' messages automatically. */
static void cmd_mute(struct sourceinfo *si, int parc, char *parv[]) {
    void* user = si->smu ? (void*)si->smu : si->su;
    
    /* Make sure they aren't muted already. */
    if (metadata_find(user, BACKLOG_MUTE_MD)) {
        command_fail(si, fault_alreadyexists, _("You are already muted."));
    }

    /* Deafen them. */
    if (!metadata_add(user, BACKLOG_MUTE_MD, "true")) {
        command_fail(si, fault_internalerror, _("An internal error occured in %s at line %d."), __FILE__, __LINE__);
        return;
    }
    command_success_nodata(si, _("You have been muted. %s will no longer pay attention to you."), backlog.me->nick);
}

/* Allow BacklogServ to record users' messages automatically. */
static void cmd_unmute(struct sourceinfo *si, int parc, char *parv[]) {
    void* user = si->smu ? (void*)si->smu : si->su;
    
    /* Make sure they aren't unmuted already. */
    if (!metadata_find(user, BACKLOG_MUTE_MD)) {
        command_fail(si, fault_alreadyexists, _("You are already unmuted."));
    }

    /* Undeafen them. */
    metadata_delete(user, BACKLOG_MUTE_MD);
    command_success_nodata(si, _("You have been unmuted. %s will record your activity."), backlog.me->nick);
}

/* Stop BacklogServ from sending the backlog to users automatically. */
static void cmd_deafen(struct sourceinfo *si, int parc, char *parv[]) {
    void* user = si->smu ? (void*)si->smu : si->su;
    
    /* Make sure they aren't deafened already. */
    if (metadata_find(user, BACKLOG_DEAFEN_MD)) {
        command_fail(si, fault_alreadyexists, _("You are already deafened."));
    }

    /* Deafen them. */
    if (!metadata_add(user, BACKLOG_DEAFEN_MD, "true")) {
        command_fail(si, fault_internalerror, _("An internal error occured in %s at line %d."), __FILE__, __LINE__);
        return;
    }
    command_success_nodata(si, _("You have been deafened. %s will not send the backlog to you."), backlog.me->nick);
}

/* Allow BacklogServ to send the backlog to users automatically. */
static void cmd_undeafen(struct sourceinfo *si, int parc, char *parv[]) {
    void* user = si->smu ? (void*)si->smu : si->su;
    
    /* Make sure they aren't undeafened already. */
    if (!metadata_find(user, BACKLOG_DEAFEN_MD)) {
        command_fail(si, fault_alreadyexists, _("You are already undeafened."));
    }

    /* Undeafen them. */
    metadata_delete(user, BACKLOG_DEAFEN_MD);
    command_success_nodata(si, _("You have been undeafened. %s will send the backlog to you."), backlog.me->nick);
}

/* Send the backlog to the user. */
static void cmd_send(struct sourceinfo *si, int parc, char *parv[]) {
    mychan_t *mc;
    char *name;
    void* user = si->smu ? (void*)si->smu : si->su;
    unsigned int length = backlogLength;

    if (parc < 1 || (name = parv[0]) == nullptr) {
        command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "SEND");
		command_fail(si, fault_needmoreparams, _("To send the backlog for a channel: SEND <#channel> [length]"));
		return;
    }

    /* Make sure the name specified is actually a channel name. */
    if (name[0] != '#') {
        command_fail(si, fault_badparams, STR_INVALID_PARAMS, "SEND");
		command_fail(si, fault_badparams, _("Syntax: %s SEND <#channel> [length]"), backlog.me->nick);
		return;
    }

    /* 
     * Make sure the channel exists and is registered so we can save metadata to it. 
     * Is this necessary? -Loganius
     */
    if (!(mc = mychan_find(name))) {
        command_fail(si, fault_badparams, STR_IS_NOT_REGISTERED, name);
        return;
    }

    /* Make sure we are currently in the channel. */
    if (!metadata_find(mc, BACKLOG_ENABLE_MD)) {
        command_fail(si, fault_badparams, _("%s is not currently in %s."), backlog.me->nick, name);
        return;
    }
    
    /* If the user requested a specific length, figure out what it is. */
    if (parc == 2 && parv[1] != nullptr) {
        length = std::atoi(parv[1]);
    } else {
        /* Otherwise, check if they have a default length, and if so, use it. */
        metadata_t *md;
        if ((md = metadata_find(user, BACKLOG_LENGTH_MD)) && md->value) {
            length = std::atoi(md->value);
        }
    }

    send_backlog(si->su, name, length, true);
    command_success_nodata(si, _("Sent the backlog for %s."), name);
}

/* Allows users to set a specific backlog length to be sent. */
static void cmd_length(struct sourceinfo *si, int parc, char *parv[]) {
    void* user = si->smu ? (void*)si->smu : si->su;
    char* len;

    /* Would people want to be able to adjust this on a per-channel basis? -Loganius */
    if (parc != 1 || (len = parv[0]) == nullptr) {
        command_fail(si, fault_needmoreparams, STR_INSUFFICIENT_PARAMS, "LENGTH");
		command_fail(si, fault_needmoreparams, _("To set the backlog length when you join a channel: LENGTH <length>"));
		return;
    }

    /* 
     * Make sure the length specified is actually a number 
     * (that can be represented by a 32bit int). 
     */
    for (size_t i = 0; len[i]; i++) {
        if (!isdigit(len[i]) || i > 8) {
            command_fail(si, fault_badparams, STR_INVALID_PARAMS, "LENGTH");
            command_fail(si, fault_badparams, _("Syntax: %s LENGTH <length:number>"), backlog.me->nick);
            return;
        }
    }

    /* Set their new length. */
    if (!metadata_add(user, BACKLOG_LENGTH_MD, len)) {
        command_fail(si, fault_internalerror, _("An internal error occured in %s at line %d."), __FILE__, __LINE__);
        return;
    }

    command_success_nodata(si, _("Backlog length successfully changed to %s."), len);
}

static void cmd_help(struct sourceinfo *si, int parc, char *parv[]) {
    command_help(si, si->service->commands);
}

static command_t backlog_join = {
    .name = "JOIN",
    .desc = N_("Adds BacklogServ to a channel."),
    .access = AC_AUTHENTICATED,
    .maxparc = 1,
    .cmd = &cmd_join,
    .help = { .path = "" },  
};

static command_t backlog_leave = {
    .name = "LEAVE",
    .desc = N_("Removes BacklogServ from a channel."),
    .access = AC_AUTHENTICATED,
    .maxparc = 1,
    .cmd = &cmd_leave,
    .help = { .path = "" },  
};

static command_t backlog_silence = {
    .name = "SILENCE",
    .desc = N_("Stops BacklogServ from automatically sending the backlog for a channel."),
    .access = AC_AUTHENTICATED,
    .maxparc = 1,
    .cmd = &cmd_silence,
    .help = { .path = "" },  
};

static command_t backlog_unsilence = {
    .name = "UNSILENCE",
    .desc = N_("Allows BacklogServ to automatically send the backlog for a channel."),
    .access = AC_AUTHENTICATED,
    .maxparc = 1,
    .cmd = &cmd_unsilence,
    .help = { .path = "" },  
};

static command_t backlog_mute = {
    .name = "MUTE",
    .desc = N_("Stops BacklogServ from listening to your activity."),
    .access = AC_NONE,
    .maxparc = 0,
    .cmd = &cmd_mute,
    .help = { .path = "" },  
};

static command_t backlog_unmute = {
    .name = "UNMUTE",
    .desc = N_("Allows BacklogServ to listen to your activity."),
    .access = AC_NONE,
    .maxparc = 0,
    .cmd = &cmd_unmute,
    .help = { .path = "" },  
};

static command_t backlog_deafen = {
    .name = "DEAFEN",
    .desc = N_("Stops BacklogServ from automatically sending you the backlog."),
    .access = AC_NONE,
    .maxparc = 0,
    .cmd = &cmd_deafen,
    .help = { .path = "" },  
};

static command_t backlog_undeafen = {
    .name = "UNDEAFEN",
    .desc = N_("Allows BacklogServ to automatically send you the backlog."),
    .access = AC_NONE,
    .maxparc = 0,
    .cmd = &cmd_undeafen,
    .help = { .path = "" },  
};

static command_t backlog_send = {
    .name = "SEND",
    .desc = N_("Sends the backlog for a channel."),
    .access = AC_NONE,
    .maxparc = 2,
    .cmd = &cmd_send,
    .help = { .path = "" },  
};

static command_t backlog_length = {
    .name = "LENGTH",
    .desc = N_("Sets the default backlog length for you."),
    .access = AC_NONE,
    .maxparc = 2,
    .cmd = &cmd_length,
    .help = { .path = "" },  
};

static command_t backlog_help = {
    .name = "HELP",
    .desc = N_("Displays this command listing."),
    .access = AC_NONE,
    .maxparc = 0,
    .cmd = &cmd_help,
    .help = { .path = "help" },
};

static void mod_init(module_t *m) {
    backlog.me = service_add("BacklogServ", NULL);
	hook_add_channel_message(on_message);
    hook_add_channel_join(on_join);
    hook_add_channel_part(on_part);
    hook_add_user_delete(on_del);

    add_uint_conf_item("LENGTH", &backlog.me->conf_table, 0, &backlogLength, 1, 1000000000, 100);

    service_bind_command(backlog.me, &backlog_join);
    service_bind_command(backlog.me, &backlog_leave);
    service_bind_command(backlog.me, &backlog_silence);
    service_bind_command(backlog.me, &backlog_unsilence);
    service_bind_command(backlog.me, &backlog_mute);
    service_bind_command(backlog.me, &backlog_unmute);
    service_bind_command(backlog.me, &backlog_deafen);
    service_bind_command(backlog.me, &backlog_undeafen);
    service_bind_command(backlog.me, &backlog_send);
    service_bind_command(backlog.me, &backlog_length);
    service_bind_command(backlog.me, &backlog_help);
}

static void mod_deinit(const module_unload_intent_t intent) {
    hook_del_channel_message(on_message);
    hook_del_channel_join(on_join);
    hook_del_channel_part(on_part);
    hook_del_user_delete(on_del);

    service_unbind_command(backlog.me, &backlog_join);
    service_unbind_command(backlog.me, &backlog_leave);
    service_unbind_command(backlog.me, &backlog_silence);
    service_unbind_command(backlog.me, &backlog_unsilence);
    service_unbind_command(backlog.me, &backlog_mute);
    service_unbind_command(backlog.me, &backlog_unmute);
    service_unbind_command(backlog.me, &backlog_deafen);
    service_unbind_command(backlog.me, &backlog_undeafen);
    service_unbind_command(backlog.me, &backlog_send);
    service_unbind_command(backlog.me, &backlog_length);
    service_unbind_command(backlog.me, &backlog_help);

	service_delete(backlog.me);
}

VENDOR_DECLARE_MODULE_V1("contrib/backlog", MODULE_UNLOAD_CAPABILITY_OK, CONTRIB_VENDOR_LOGANIUS)