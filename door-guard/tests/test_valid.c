/*
 * test_valid.c — 字段合法性规则(proto/valid.h)
 *
 * 规则是"输入检测"的唯一权威,存储层与 UI 弹窗共用:这里逐条锁死边界,
 * 避免以后改规则时只改了一处(设备上能输、库里却拒 / 反之)。
 */
#include "dg_test.h"
#include "valid.h"

#include <string.h>

static void t_uid(void)
{
    printf("[V1] user_id:长度 3~31、字母数字/-/_、首字符必须字母或数字\n");
    DG_CHECK(dg_valid_uid("10001") == DG_OK);
    DG_CHECK(dg_valid_uid("abc") == DG_OK);
    DG_CHECK(dg_valid_uid("A1-b_2") == DG_OK);
    DG_CHECK(dg_valid_uid("abc") == DG_OK);

    DG_CHECK(dg_valid_uid(NULL) == DG_ERR_BAD_UID);
    DG_CHECK(dg_valid_uid("") == DG_ERR_BAD_UID);
    DG_CHECK(dg_valid_uid("12") == DG_ERR_BAD_UID);          /* 太短 */
    DG_CHECK(dg_valid_uid("-abc") == DG_ERR_BAD_UID);        /* 不能以 - 开头 */
    DG_CHECK(dg_valid_uid("_abc") == DG_ERR_BAD_UID);        /* 不能以 _ 开头 */
    DG_CHECK(dg_valid_uid("张 三") == DG_ERR_BAD_UID);        /* 中文/空格非法 */
    DG_CHECK(dg_valid_uid("ab cd") == DG_ERR_BAD_UID);
    DG_CHECK(dg_valid_uid("abc;rm -rf") == DG_ERR_BAD_UID);  /* 命令注入字符 */
    DG_CHECK(dg_valid_uid("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == DG_ERR_BAD_UID); /* 32 位 */
    DG_CHECK(dg_valid_uid("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == DG_OK);           /* 31 位 */
}

static void t_name(void)
{
    printf("[V2] user_name:1~63 字节、非空、无前后空格/控制字符\n");
    DG_CHECK(dg_valid_name("张三") == DG_OK);
    DG_CHECK(dg_valid_name("Zhang San") == DG_OK);
    DG_CHECK(dg_valid_name("A") == DG_OK);
    DG_CHECK(dg_valid_name("张三·李四") == DG_OK);            /* 中点允许 */

    DG_CHECK(dg_valid_name(NULL) == DG_ERR_BAD_NAME);
    DG_CHECK(dg_valid_name("") == DG_ERR_BAD_NAME);
    DG_CHECK(dg_valid_name(" 张三") == DG_ERR_BAD_NAME);      /* 前导空格 */
    DG_CHECK(dg_valid_name("张三 ") == DG_ERR_BAD_NAME);      /* 尾随空格 */
    DG_CHECK(dg_valid_name("a\tb") == DG_ERR_BAD_NAME);       /* 控制字符 */
}

static void t_pwd(void)
{
    printf("[V3] password:4~31 位可见 ASCII、无空格\n");
    DG_CHECK(dg_valid_pwd("1234") == DG_OK);
    DG_CHECK(dg_valid_pwd("pwd1") == DG_OK);
    DG_CHECK(dg_valid_pwd("Abc123!@#") == DG_OK);
    DG_CHECK(dg_valid_pwd("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == DG_OK); /* 31 位 */

    DG_CHECK(dg_valid_pwd(NULL) == DG_ERR_BAD_PWD);
    DG_CHECK(dg_valid_pwd("") == DG_ERR_BAD_PWD);
    DG_CHECK(dg_valid_pwd("123") == DG_ERR_BAD_PWD);          /* 太短 */
    DG_CHECK(dg_valid_pwd("pass word") == DG_ERR_BAD_PWD);    /* 空格 */
    DG_CHECK(dg_valid_pwd("密码1234") == DG_ERR_BAD_PWD);      /* 中文(键盘打不出) */
    DG_CHECK(dg_valid_pwd("aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa") == DG_ERR_BAD_PWD);
}

static void t_hint(void)
{
    printf("[V4] 失败原因说明可读(日志/上位机用)\n");
    DG_CHECK(dg_valid_hint(DG_ERR_BAD_UID) && *dg_valid_hint(DG_ERR_BAD_UID));
    DG_CHECK(dg_valid_hint(DG_ERR_BAD_NAME) && *dg_valid_hint(DG_ERR_BAD_NAME));
    DG_CHECK(dg_valid_hint(DG_ERR_BAD_PWD) && *dg_valid_hint(DG_ERR_BAD_PWD));
    DG_CHECK(strcmp(dg_valid_hint(0), "取值非法") != 0 || 1);   /* 未知码不崩 */
}

int main(void)
{
    t_uid();
    t_name();
    t_pwd();
    t_hint();
    DG_TEST_EXIT();
}
