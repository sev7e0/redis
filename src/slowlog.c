/* Slowlog implements a system that is able to remember the latest N queries that took more than M microseconds to execute.
 *
 * The execution time to reach to be logged in the slow log is set
 * using the 'slowlog-log-slower-than' config directive, that is also
 * readable and writable using the CONFIG SET/GET command.
 *
 * The slow queries log is actually not "logged" in the Redis log file
 * but is accessible thanks to the SLOWLOG command.
 *
 * ----------------------------------------------------------------------------
 * slowlog慢日志是redis中一个的系统，他能够记住redis中最近的m毫秒内，n个被执行的查询。
 *
 * 使用'slowlog-log-slow-than'config指令设置要记录在慢速日志中的执行时间，该指令也
 * 可以使用CONFIG SET / GET命令进行读写。
 *
 * 慢查询日志的实现并没有使用redis的日志文件，能够查询慢日志是因为SLOWLOG指令
 *
 */


#include "server.h"
#include "slowlog.h"

/* Create a new slowlog entry.
 * Incrementing the ref count of all the objects retained is up to
 * this function.
 * ------------------------------------------------------------
 * 创建一个新的慢日志entry
 *
 * 增加保留的所有对象的引用计数取决于此功能。
 */
slowlogEntry *slowlogCreateEntry(client *c, robj **argv, int argc, long long duration) {
    slowlogEntry *se = zmalloc(sizeof(*se));
    int j, slargc = argc;

    if (slargc > SLOWLOG_ENTRY_MAX_ARGC) slargc = SLOWLOG_ENTRY_MAX_ARGC;
    se->argc = slargc;
    se->argv = zmalloc(sizeof(robj*)*slargc);
    for (j = 0; j < slargc; j++) {
        /* Logging too many arguments is a useless memory waste, so we stop
         * at SLOWLOG_ENTRY_MAX_ARGC, but use the last argument to specify
         * how many remaining arguments there were in the original command. */
        if (slargc != argc && j == slargc-1) {
            se->argv[j] = createObject(OBJ_STRING,
                sdscatprintf(sdsempty(),"... (%d more arguments)",
                argc-slargc+1));
        } else {
            /* Trim too long strings as well... */
            if (argv[j]->type == OBJ_STRING &&
                sdsEncodedObject(argv[j]) &&
                sdslen(argv[j]->ptr) > SLOWLOG_ENTRY_MAX_STRING)
            {
                sds s = sdsnewlen(argv[j]->ptr, SLOWLOG_ENTRY_MAX_STRING);

                s = sdscatprintf(s,"... (%lu more bytes)",
                    (unsigned long)
                    sdslen(argv[j]->ptr) - SLOWLOG_ENTRY_MAX_STRING);
                se->argv[j] = createObject(OBJ_STRING,s);
            } else if (argv[j]->refcount == OBJ_SHARED_REFCOUNT) {
                se->argv[j] = argv[j];
            } else {
                /* Here we need to dupliacate the string objects composing the
                 * argument vector of the command, because those may otherwise
                 * end shared with string objects stored into keys. Having
                 * shared objects between any part of Redis, and the data
                 * structure holding the data, is a problem: FLUSHALL ASYNC
                 * may release the shared string object and create a race. */
                se->argv[j] = dupStringObject(argv[j]);
            }
        }
    }
    se->time = time(NULL);
    se->duration = duration;
    se->id = server.slowlog_entry_id++;
    se->peerid = sdsnew(getClientPeerId(c));
    se->cname = c->name ? sdsnew(c->name->ptr) : sdsempty();
    return se;
}

/* Free a slow log entry. The argument is void so that the prototype of this function matches the one of the 'free' method of adlist.c.
 *
 * This function will take care to release all the retained object.
 * ----------------------------------------------------------------
 * 释放一个slowlog entry，该函数的原型与adlist.c的'free'方法匹配。
 *
 * 此功能将注意释放所有保留的对象。
 * -*/
void slowlogFreeEntry(void *septr) {
    slowlogEntry *se = septr;
    int j;

    for (j = 0; j < se->argc; j++)
        decrRefCount(se->argv[j]);
    zfree(se->argv);
    sdsfree(se->peerid);
    sdsfree(se->cname);
    zfree(se);
}

/* Initialize the slow log. This function should be called a single time
 * at server startup.
 * ---------------------------------------------------------------------
 * 初始化slow log该函数只有在server启动时调用
 * */
void slowlogInit(void) {
    server.slowlog = listCreate();
    server.slowlog_entry_id = 0;
    listSetFreeMethod(server.slowlog,slowlogFreeEntry);
}

/* Push a new entry into the slow log.
 * This function will make sure to trim the slow log accordingly to the
 * configured max length.
 * ---------------------------------------------------------------------
 * 将一个新的entry添加到慢日志中去
 *
 * 该函数将会根据配置的slowlog_max_len去修剪slowlog中的日志
 *
 * */

void slowlogPushEntryIfNeeded(client *c, robj **argv, int argc, long long duration) {
    //检测slowlog是否开启
    if (server.slowlog_log_slower_than < 0) return; /* Slowlog disabled */
    if (duration >= server.slowlog_log_slower_than)
        /*
         * 从头部开始添加
         */
        listAddNodeHead(server.slowlog,
                        slowlogCreateEntry(c,argv,argc,duration));

    /* Remove old entries if needed.
     * -----------------------------
     * 若当前的list长度大于配置的配置的长度，那么将会进行删除（尾部删除）
     * */
    while (listLength(server.slowlog) > server.slowlog_max_len)
        listDelNode(server.slowlog,listLast(server.slowlog));
}

/* Remove all the entries from the current slow log.
 * ------------------------------------------------
 * 循环删除server.slowlog list中的节点
 * */
void slowlogReset(void) {
    while (listLength(server.slowlog) > 0)
        listDelNode(server.slowlog,listLast(server.slowlog));
}

/* The SLOWLOG command. Implements all the subcommands needed to handle the Redis slow log.
 * -----------------------------------------------------------------
 *
 * slowlog的命令实现，包含了slow日志所需要的所有子命令
 *
 * */
void slowlogCommand(client *c) {
    if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"help")) {
        const char *help[] = {
"GET [count] -- Return top entries from the slowlog (default: 10)."
"    Entries are made of:",
"    id, timestamp, time in microseconds, arguments array, client IP and port, client name",
"LEN -- Return the length of the slowlog.",
"RESET -- Reset the slowlog.",
NULL
        };
        addReplyHelp(c, help);
        /*
         * 重置命令
         */
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"reset")) {
        slowlogReset();
        addReply(c,shared.ok);
        /*
         * len命令，求出长度
         */
    } else if (c->argc == 2 && !strcasecmp(c->argv[1]->ptr,"len")) {
        addReplyLongLong(c,listLength(server.slowlog));
        /*
         * get命令
         */
    } else if ((c->argc == 2 || c->argc == 3) &&
               !strcasecmp(c->argv[1]->ptr,"get"))
    {
        long count = 10, sent = 0;
        listIter li;
        void *totentries;
        listNode *ln;
        slowlogEntry *se;

        if (c->argc == 3 &&
            getLongFromObjectOrReply(c,c->argv[2],&count,NULL) != C_OK)
            return;
        /*
         * 将server.slowlog的list构造成迭代器
         */
        listRewind(server.slowlog,&li);
        totentries = addDeferredMultiBulkLength(c);
        /*
         * 开始遍历，打印出结果
         */
        while(count-- && (ln = listNext(&li))) {
            int j;

            se = ln->value;
            addReplyMultiBulkLen(c,6);
            addReplyLongLong(c,se->id);
            addReplyLongLong(c,se->time);
            addReplyLongLong(c,se->duration);
            addReplyMultiBulkLen(c,se->argc);
            for (j = 0; j < se->argc; j++)
                addReplyBulk(c,se->argv[j]);
            addReplyBulkCBuffer(c,se->peerid,sdslen(se->peerid));
            addReplyBulkCBuffer(c,se->cname,sdslen(se->cname));
            sent++;
        }
        setDeferredMultiBulkLength(c,totentries,sent);
    } else {
        addReplySubcommandSyntaxError(c);
    }
}
