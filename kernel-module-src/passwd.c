#include <linux/kernel.h>
#include <linux/module.h>
#include <crypto/hash.h>

#include <passwd.h>

//https://www.kernel.org/doc/html/v5.0/crypto/api-digest.html#synchronous-message-digest-api    
//https://www.kernel.org/doc/html/v4.14/crypto/api-samples.html#code-example-for-use-of-operational-state-memory-with-shash

#ifdef CONFIG_SYSFS
static char auth_passwd_salt[32];
static struct crypto_shash *sha256_shash;
#endif

static char *auth_passwd;
extern char* activation_ct_passwd;

#ifdef CONFIG_SYSFS

static int hash_sha256(const char* data, size_t datalen, char* output) {
    size_t size = sizeof(struct shash_desc) + crypto_shash_descsize(sha256_shash);

    struct shash_desc *desc = kmalloc(size, GFP_KERNEL);
    if (desc == NULL) {
        return -ENOMEM;
    }
    
    desc->tfm = sha256_shash;

    size_t datasize = sizeof(char) * strlen(data);
    size_t saltsize = sizeof(auth_passwd_salt);

    char* data_with_salt = kmalloc(datasize + saltsize, GFP_KERNEL);
    if(data_with_salt == NULL) {
        kfree(desc);
        return -ENOENT;
    }

    memcpy(data_with_salt, data, datasize);
    memcpy(data_with_salt + datasize, auth_passwd_salt, saltsize);

    int rv = crypto_shash_digest(desc, data_with_salt, datasize + saltsize, output);

    kfree(desc);
    kfree(data_with_salt);

    return rv;
}

#endif

int password_cmp(const char* passwd) {

#ifdef CONFIG_SYSFS
    if(sha256_shash != NULL) {
        char hashed_passwd[32];
        int rv = hash_sha256(passwd, strlen(passwd), hashed_passwd);
        if(rv != 0) {
            pr_err("hash_sha256 has failed: %d\n", rv);
            return rv;
        }

        return memcmp(hashed_passwd, auth_passwd, 32);
    } else {
#endif
        return strcmp(passwd, auth_passwd);

#ifdef CONFIG_SYSFS
    }
#endif
}

void destroy_passwd(void) {
    if(auth_passwd != NULL) {
        kfree(auth_passwd);
        auth_passwd = NULL;
    }

#ifdef CONFIG_SYSFS
    if (sha256_shash != NULL) {
        crypto_free_shash(sha256_shash);
        sha256_shash = NULL;
    }
#endif
}

int setup_passwd(void) {

#ifdef CONFIG_SYSFS
    sha256_shash = crypto_alloc_shash("sha256", 0, 0);
    if (!IS_ERR(sha256_shash)) {
        auth_passwd = kmalloc(32 * sizeof(char), GFP_KERNEL);
        if(auth_passwd == NULL) {
            return -ENOMEM;
        }

        //https://elixir.bootlin.com/linux/v6.16/source/include/linux/random.h#L126
        //https://elixir.bootlin.com/linux/v6.16/source/drivers/char/random.c#L137
        while(get_random_bytes_wait(auth_passwd_salt, 32) == -ERESTARTSYS)
            ;

        if(hash_sha256(activation_ct_passwd, strlen(activation_ct_passwd), auth_passwd) != 0) {
            return -1;
        }
    } else {
        pr_warn("unable to allocate sha256_shash: %ld\n", PTR_ERR(sha256_shash));
        sha256_shash = NULL;
#endif
        auth_passwd = kmalloc(strlen(activation_ct_passwd) * sizeof(char), GFP_KERNEL);
        if(auth_passwd == NULL) {
            return -ENOMEM;
        } 

        memcpy(auth_passwd, activation_ct_passwd, strlen(activation_ct_passwd) * sizeof(char));

#ifdef CONFIG_SYSFS
    }

    memset(activation_ct_passwd, 0, strlen(activation_ct_passwd) * sizeof(char));
#endif

    return 0;
}
