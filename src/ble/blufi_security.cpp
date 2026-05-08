// BluFi BLE 配网安全实现：DH 密钥协商 + AES-128 CBC 加解密 + CRC-16 校验
// 与 ESP-IDF BluFi 示例（examples/bluetooth/blufi）协议兼容。
// EspBluFi App 每次连接必须先完成 DH 协商后才会发送 SSID/密码，
// 若 negotiate_data_handler 为 nullptr 则协商无响应，App 永远卡在 loading。
#include "blufi_security.h"
#include "../logger/logger.h"
#include <string.h>
#include <stdlib.h>
#include <mbedtls/dhm.h>
#include <mbedtls/aes.h>
#include <esp_crc.h>
#include <esp_random.h>

#define SHARE_KEY_LEN 128
#define PSK_LEN       16

static mbedtls_dhm_context s_dhm;
static uint8_t             s_psk[PSK_LEN];
static bool                s_negotiated = false;

static int s_hwRandom(void*, unsigned char *buf, size_t len) {
    esp_fill_random(buf, len);
    return 0;
}

void blufi_security_reset() {
    // mbedtls_dhm_free 对零初始化的上下文（所有 MPI 指针为 null）是安全的
    mbedtls_dhm_free(&s_dhm);
    mbedtls_dhm_init(&s_dhm);
    memset(s_psk, 0, PSK_LEN);
    s_negotiated = false;
}

void blufi_dh_negotiate_data_handler(uint8_t *data, int len,
                                      uint8_t **output_data, int *output_len,
                                      bool *need_free) {
    *output_data = nullptr;
    *output_len  = 0;
    *need_free   = false;

    if (s_negotiated) return;  // 已完成协商，忽略重复数据

    // 解析手机发来的 DH 参数（ServerDHParams 格式：P + G + Phone 公钥，各含 2 字节长度前缀）
    const uint8_t *end = data + len;
    if (mbedtls_dhm_read_params(&s_dhm, &data, end) != 0) {
        LOG("BluFi", "DH read_params 失败");
        return;
    }

    size_t   keyLen  = s_dhm.len;  // mbedtls 2.x 直接字段访问（3.x 用 mbedtls_dhm_get_len）
    uint8_t *selfPub = (uint8_t*)malloc(keyLen);
    if (!selfPub) return;

    if (mbedtls_dhm_make_public(&s_dhm, (int)keyLen, selfPub, keyLen, s_hwRandom, nullptr) != 0) {
        LOG("BluFi", "DH make_public 失败");
        free(selfPub);
        return;
    }

    uint8_t shareKey[SHARE_KEY_LEN];
    size_t  shareLen = SHARE_KEY_LEN;
    if (mbedtls_dhm_calc_secret(&s_dhm, shareKey, SHARE_KEY_LEN, &shareLen, s_hwRandom, nullptr) != 0) {
        LOG("BluFi", "DH calc_secret 失败");
        free(selfPub);
        return;
    }

    // 取共享密钥前 16 字节作为 AES-128 密钥
    memcpy(s_psk, shareKey, PSK_LEN);
    mbedtls_dhm_free(&s_dhm);
    s_negotiated = true;

    *output_data = selfPub;
    *output_len  = (int)keyLen;
    *need_free   = true;
    LOG("BluFi", "DH 协商完成，AES 密钥已就绪");
}

int blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_enc(&aes, s_psk, PSK_LEN * 8);

    uint8_t iv[16] = {};
    iv[0] = iv8;

    // BluFi 调用方已分配 16 字节额外空间，补零填充至 16 字节倍数
    int padLen   = (16 - (crypt_len % 16)) % 16;
    int totalLen = crypt_len + padLen;
    memset(crypt_data + crypt_len, 0, padLen);

    // CBC 模式：手动 XOR + ECB 加密
    uint8_t tmp[16], out[16];
    for (int i = 0; i < totalLen; i += 16) {
        for (int j = 0; j < 16; j++) tmp[j] = crypt_data[i + j] ^ iv[j];
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_ENCRYPT, tmp, out);
        memcpy(crypt_data + i, out, 16);
        memcpy(iv, out, 16);
    }

    mbedtls_aes_free(&aes);
    return totalLen;
}

int blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len) {
    mbedtls_aes_context aes;
    mbedtls_aes_init(&aes);
    mbedtls_aes_setkey_dec(&aes, s_psk, PSK_LEN * 8);

    uint8_t iv[16] = {};
    iv[0] = iv8;

    uint8_t dec[16], prev[16];
    for (int i = 0; i < crypt_len; i += 16) {
        memcpy(prev, crypt_data + i, 16);
        mbedtls_aes_crypt_ecb(&aes, MBEDTLS_AES_DECRYPT, crypt_data + i, dec);
        for (int j = 0; j < 16; j++) dec[j] ^= iv[j];
        memcpy(crypt_data + i, dec, 16);
        memcpy(iv, prev, 16);
    }

    mbedtls_aes_free(&aes);
    return crypt_len;
}

uint16_t blufi_crc_checksum(uint8_t /*iv8*/, uint8_t *data, int len) {
    return esp_crc16_le(UINT16_MAX, data, (uint32_t)len);
}
