#pragma once
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/// 每次 BLE 连接建立时调用，重置 DH 协商状态
void     blufi_security_reset();

/// BluFi DH 密钥协商处理器（ServerDHParams 格式）
void     blufi_dh_negotiate_data_handler(uint8_t *data, int len,
                                          uint8_t **output_data, int *output_len,
                                          bool *need_free);

/// AES-128 CBC 加密（in-place）；返回加密后长度（已补齐至 16 字节倍数）
int      blufi_aes_encrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

/// AES-128 CBC 解密（in-place）；返回解密后长度
int      blufi_aes_decrypt(uint8_t iv8, uint8_t *crypt_data, int crypt_len);

/// CRC-16/IBM 校验和
uint16_t blufi_crc_checksum(uint8_t iv8, uint8_t *data, int len);

#ifdef __cplusplus
}
#endif
