#include <hiredis/hiredis.h>
#include <stdio.h>
#include <stdlib.h>

int main() {
    const char *host = getenv("REDIS_HOST");
    redisContext *c = redisConnect(host ? host : "redis", 6379);
    if (c == NULL || c->err) {
        printf("connect failed: %s\n", c ? c->errstr : "?");
        return 1;
    }
    puts("connected");

    // does hiredis' format parser accept %zu?
    redisReply *r = (redisReply *)redisCommand(c, "HSET lob:hr:probe size %zu volume %lld",
                                               (size_t)3, 5LL);
    if (r == NULL) {
        printf("null reply with %%%%zu; ctx err=%d (%s)\n", c->err, c->errstr);
    } else {
        printf("hset reply type=%d (int=%lld) ctx err=%d\n", r->type,
               r->type == REDIS_REPLY_INTEGER ? r->integer : -1, c->err);
        freeReplyObject(r);
    }

    // blocking XREADGROUP sanity (group created first)
    r = (redisReply *)redisCommand(c, "XGROUP CREATE lob:hr:probe3 grp $ MKSTREAM");
    if (r != NULL) freeReplyObject(r);  // ignore BUSYGROUP/etc.
    r = (redisReply *)redisCommand(
        c, "XREADGROUP GROUP grp c1 BLOCK 1000 COUNT 8 STREAMS lob:hr:probe3 >");
    if (r == NULL) {
        printf("xreadgroup null; ctx err=%d (%s)\n", c->err, c->errstr);
    } else {
        printf("xreadgroup type=%d\n", r->type);
        freeReplyObject(r);
    }
    redisFree(c);
    return 0;
}
