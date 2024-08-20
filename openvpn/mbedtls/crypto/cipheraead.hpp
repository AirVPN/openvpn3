//    OpenVPN -- An application to securely tunnel IP networks
//               over a single port, with support for SSL/TLS-based
//               session authentication and key exchange,
//               packet encryption, packet authentication, and
//               packet compression.
//
//    Copyright (C) 2012-2022 OpenVPN Inc.
//
//    This program is free software: you can redistribute it and/or modify
//    it under the terms of the GNU Affero General Public License Version 3
//    as published by the Free Software Foundation.
//
//    This program is distributed in the hope that it will be useful,
//    but WITHOUT ANY WARRANTY; without even the implied warranty of
//    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
//    GNU Affero General Public License for more details.
//
//    You should have received a copy of the GNU Affero General Public License
//    along with this program in the COPYING file.
//    If not, see <http://www.gnu.org/licenses/>.

// Wrap the mbed TLS AEAD API.

#pragma once
#include <string>

#include <mbedtls/gcm.h>
#include <mbedtls/chachapoly.h>

#include <openvpn/common/size.hpp>
#include <openvpn/common/exception.hpp>
#include <openvpn/common/likely.hpp>
#include <openvpn/crypto/static_key.hpp>
#include <openvpn/crypto/cryptoalgs.hpp>

namespace openvpn::MbedTLSCrypto {
class CipherContextAEAD : public CipherContextCommon
{
    CipherContextAEAD(const CipherContextAEAD&) = delete;
    CipherContextAEAD& operator=(const CipherContextAEAD&) = delete;

    private:

        bool initialized;
        mbedtls_gcm_context gcm_ctx;
        mbedtls_chachapoly_context chachapoly_ctx;
        CryptoAlgs::Type crypto_alg;
        
        void (openvpn::MbedTLSCrypto::CipherContextAEAD::*aead_encrypt)(const unsigned char *input, unsigned char *output, size_t length, const unsigned char *iv, unsigned char *tag, const unsigned char *ad, size_t ad_len);
        bool (openvpn::MbedTLSCrypto::CipherContextAEAD::*aead_decrypt)(const unsigned char *input, unsigned char *output, size_t length, const unsigned char *iv, const unsigned char *tag, const unsigned char *ad, size_t ad_len);

    public:

        OPENVPN_EXCEPTION(mbedtls_aead_error);

    CipherContextAEAD() = default;

    ~CipherContextAEAD()
    {
        erase();
    }

    void init(SSLLib::Ctx libctx,
              const CryptoAlgs::Type alg,
              const unsigned char *key,
              const unsigned int keysize,
              const int mode)
    {
        erase();

        check_mode(mode);

        // get cipher type
        unsigned int ckeysz = 0;
        const mbedtls_cipher_type_t cid = cipher_type(alg, ckeysz);
        if (cid == MBEDTLS_CIPHER_NONE)
            OPENVPN_THROW(mbedtls_aead_error, CryptoAlgs::name(alg) << ": not usable");

        if (ckeysz > keysize)
            throw mbedtls_aead_error("insufficient key material");

        auto *ci = mbedtls_cipher_info_from_type(cid);

        // initialize cipher context with cipher type
        if (mbedtls_cipher_setup(&ctx, ci) < 0)
            throw mbedtls_aead_error("mbedtls_cipher_setup");

        if (mbedtls_cipher_setkey(&ctx, key, ckeysz * 8, (mbedtls_operation_t)mode) < 0)
            throw mbedtls_aead_error("mbedtls_cipher_setkey");

        initialized = true;
    }



    void encrypt(const unsigned char *input,
                 unsigned char *output,
                 size_t length,
                 const unsigned char *iv,
                 unsigned char *tag,
                 const unsigned char *ad,
                 size_t ad_len)
    {
        check_initialized();
        const int status = mbedtls_cipher_auth_encrypt_ext(&ctx,
                                                           iv,
                                                           IV_LEN,
                                                           ad,
                                                           ad_len,
                                                           input,
                                                           length,
                                                           output,
                                                           length + AUTH_TAG_LEN,
                                                           &length,
                                                           AUTH_TAG_LEN);
        if (unlikely(status))
            OPENVPN_THROW(mbedtls_aead_error, "mbedtls_cipher_auth_encrypt failed with status=" << status);
    }

    /**
     * Decrypts AEAD encrypted data. Note that this method ignores the tag parameter
     * and the tag is assumed to be part of input and at the end of the input.
     *
     * @param input     Input data to decrypt
     * @param output    Where decrypted data will be written to
     * @param iv        IV of the encrypted data.
     * @param length    length the of the data, this includes the tag at the end.
     * @param ad        start of the additional data
     * @param ad_len    length of the additional data
     * @param tag       ignored by the mbed TLS variant of the method. (see OpenSSL variant of the method for more details).
     *
     * input and output may NOT be equal
     */
    bool decrypt(const unsigned char *input,
                 unsigned char *output,
                 size_t length,
                 const unsigned char *iv,
                 const unsigned char *tag,
                 const unsigned char *ad,
                 size_t ad_len)
    {
        check_initialized();

        if (unlikely(tag != nullptr))
        {
            /* If we are called with a non-null tag, the function is not going to be able to decrypt */
            throw mbedtls_aead_error("tag must be null for aead decrypt");
        }

        size_t olen;
        const int status = mbedtls_cipher_auth_decrypt_ext(&ctx,
                                                           iv,
                                                           IV_LEN,
                                                           ad,
                                                           ad_len,
                                                           input,
                                                           length,
                                                           output,
                                                           length - AUTH_TAG_LEN,
                                                           &olen,
                                                           AUTH_TAG_LEN);

        return (olen == length - AUTH_TAG_LEN) && (status == 0);
    }

    bool is_initialized() const
    {
        return initialized;
    }

    static bool is_supported(void *libctx, const CryptoAlgs::Type alg)
    {
        unsigned int keysize;
        return (cipher_type(alg, keysize) != MBEDTLS_CIPHER_NONE);
    }

  private:
    static mbedtls_cipher_type_t cipher_type(const CryptoAlgs::Type alg, unsigned int &keysize)
    {
        switch (alg)
        {
            MODE_UNDEF = MBEDTLS_OPERATION_NONE,
            ENCRYPT = MBEDTLS_ENCRYPT,
            DECRYPT = MBEDTLS_DECRYPT
        };

        // mbed TLS cipher constants

        enum
        {
            IV_LEN = 12,
            AUTH_TAG_LEN = 16,
            SUPPORTS_IN_PLACE_ENCRYPT = 1,
        };

        bool constexpr requires_authtag_at_end()
        {
            return false;
        }

        CipherContextAEAD()	: initialized(false)
        {
        }

        ~CipherContextAEAD()
        {
            erase();
        }

        void init(SSLLib::Ctx libctx,
                const CryptoAlgs::Type alg,
                const unsigned char *key,
                const unsigned int keysize,
                const int mode)
        {
            erase();
            
            crypto_alg = alg;

            // get cipher type
            
            unsigned int ckeysz = 0;

            const mbedtls_cipher_id_t cid = cipher_type(alg, ckeysz);

            if (cid == MBEDTLS_CIPHER_ID_NONE)
                OPENVPN_THROW(mbedtls_aead_error, CryptoAlgs::name(alg) << ": not usable");

            if(ckeysz > keysize)
                throw mbedtls_aead_error("insufficient key material");

            // initialize cipher context
            
            switch(crypto_alg)
            {
                case CryptoAlgs::AES_128_GCM:
                case CryptoAlgs::AES_192_GCM:
                case CryptoAlgs::AES_256_GCM:
                {
                    mbedtls_gcm_init(&gcm_ctx);
            
                    if(mbedtls_gcm_setkey(&gcm_ctx, cid, key, ckeysz * 8) < 0)
                        throw mbedtls_aead_error("mbedtls_gcm_setkey");

                    aead_encrypt = &openvpn::MbedTLSCrypto::CipherContextAEAD::gcm_encrypt;
                    aead_decrypt = &openvpn::MbedTLSCrypto::CipherContextAEAD::gcm_decrypt;
                }
                break;
                
                case CryptoAlgs::CHACHA20_POLY1305:
                {
                    mbedtls_chachapoly_init(&chachapoly_ctx);

                    if(mbedtls_chachapoly_setkey(&chachapoly_ctx, key) < 0)
                        throw mbedtls_aead_error("mbedtls_chachapoly_setkey");

                    aead_encrypt = &openvpn::MbedTLSCrypto::CipherContextAEAD::chachapoly_encrypt;
                    aead_decrypt = &openvpn::MbedTLSCrypto::CipherContextAEAD::chachapoly_decrypt;
                }
                break;
                
                default:
                {
                    OPENVPN_THROW(mbedtls_aead_error, CryptoAlgs::name(crypto_alg) << ": not usable");
                }
                break;
            }

            initialized = true;
        }

        void encrypt(const unsigned char *input,
                     unsigned char *output,
                     size_t length,
                     const unsigned char *iv,
                     unsigned char *tag,
                     const unsigned char *ad,
                     size_t ad_len)
        {
            check_initialized();

            (this->*aead_encrypt)(input, output, length, iv, tag, ad, ad_len);
        }

        // input and output may NOT be equal

        bool decrypt(const unsigned char *input,
                     unsigned char *output,
                     size_t length,
                     const unsigned char *iv,
                     const unsigned char *tag,
                     const unsigned char *ad,
                     size_t ad_len)
        {
            check_initialized();

            return (this->*aead_decrypt)(input, output, length, iv, tag, ad, ad_len);
        }

        bool is_initialized() const
        {
            return initialized;
        }

        static bool is_supported(void *libctx, const CryptoAlgs::Type alg)
        {
            unsigned int keysize;

            return (cipher_type(alg, keysize) != MBEDTLS_CIPHER_ID_NONE);
        }

        static bool is_supported(const CryptoAlgs::Type alg)
        {
            unsigned int keysize;

            return(cipher_type(alg, keysize) != MBEDTLS_CIPHER_ID_NONE);
        }

private:

        static mbedtls_cipher_id_t cipher_type(const CryptoAlgs::Type alg, unsigned int& keysize)
        {
            switch(alg)
            {
                case CryptoAlgs::AES_128_GCM:
                {
                    keysize = 16;
                    
                    return MBEDTLS_CIPHER_ID_AES;
                }
                break;

                case CryptoAlgs::AES_192_GCM:
                {
                    keysize = 24;

                    return MBEDTLS_CIPHER_ID_AES;
                }
                break;

                case CryptoAlgs::AES_256_GCM:
                {
                    keysize = 32;
            
                    return MBEDTLS_CIPHER_ID_AES;
                }
                break;

                case CryptoAlgs::CHACHA20_POLY1305:
                {
                    keysize = 32;
            
                    return MBEDTLS_CIPHER_ID_CHACHA20;
                }
                break;
                
                default:
                {
                    OPENVPN_THROW(mbedtls_aead_error, CryptoAlgs::name(alg) << ": not usable");
                }
                break;
            }
        }

        void erase()
        {
            if(initialized)
            {
                switch(crypto_alg)
                {
                    case CryptoAlgs::AES_128_GCM:
                    case CryptoAlgs::AES_192_GCM:
                    case CryptoAlgs::AES_256_GCM:
                    {
                        mbedtls_gcm_free(&gcm_ctx);
                    }
                    break;

                    case CryptoAlgs::CHACHA20_POLY1305:
                    {
                        mbedtls_chachapoly_free(&chachapoly_ctx);
                    }
                    break;
                    
                    default:
                    {
                    }
                    break;
                }

                initialized = false;
            }
        }

        void check_initialized() const
        {
            if(unlikely(!initialized))
                throw mbedtls_aead_error("uninitialized");
        }

        // specific ciphers methods to be assigned to private members aead_encrypt and aead_decrypt according to the cipher algorithm

        void gcm_encrypt(const unsigned char *input,
                         unsigned char *output,
                         size_t length,
                         const unsigned char *iv,
                         unsigned char *tag,
                         const unsigned char *ad,
                         size_t ad_len)
        {
            const int status = mbedtls_gcm_crypt_and_tag(&gcm_ctx,
                                                         MBEDTLS_GCM_ENCRYPT,
                                                         length,
                                                         iv,
                                                         IV_LEN,
                                                         ad,
                                                         ad_len,
                                                         input,
                                                         output,
                                                         AUTH_TAG_LEN,
                                                         tag);

            if(unlikely(status))
                OPENVPN_THROW(mbedtls_aead_error, "mbedtls_gcm_crypt_and_tag failed with status=" << status);
        }

        bool gcm_decrypt(const unsigned char *input,
                         unsigned char *output,
                         size_t length,
                         const unsigned char *iv,
                         const unsigned char *tag,
                         const unsigned char *ad,
                         size_t ad_len)
        {
            int status = mbedtls_gcm_auth_decrypt(&gcm_ctx,
                                                  length,
                                                  iv,
                                                  IV_LEN,
                                                  ad,
                                                  ad_len,
                                                  tag,
                                                  AUTH_TAG_LEN,
                                                  input,
                                                  output);

            return status == 0;
        }

        void chachapoly_encrypt(const unsigned char *input,
                                unsigned char *output,
                                size_t length,
                                const unsigned char *iv,
                                unsigned char *tag,
                                const unsigned char *ad,
                                size_t ad_len)
        {
            const int status = mbedtls_chachapoly_encrypt_and_tag(&chachapoly_ctx,
                                                                  length,
                                                                  iv,
                                                                  ad,
                                                                  ad_len,
                                                                  input,
                                                                  output,
                                                                  tag);

            if(unlikely(status))
                OPENVPN_THROW(mbedtls_aead_error, "mbedtls_chachapoly_encrypt_and_tag failed with status=" << status);
        }
    
        bool chachapoly_decrypt(const unsigned char *input,
                                unsigned char *output,
                                size_t length,
                                const unsigned char *iv,
                                const unsigned char *tag,
                                const unsigned char *ad,
                                size_t ad_len)
        {
            int status = mbedtls_chachapoly_auth_decrypt(&chachapoly_ctx,
                                                         length,
                                                         iv,
                                                         ad,
                                                         ad_len,
                                                         tag,
                                                         input,
                                                         output);

            return status == 0;
        }
};
} // namespace openvpn::MbedTLSCrypto
