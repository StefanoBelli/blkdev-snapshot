#include <linux/kernel.h>
#include <linux/module.h>
#include <crypto/hash.h>

#include <passwd.h>

//https://www.kernel.org/doc/html/v5.0/crypto/api-digest.html#synchronous-message-digest-api    
//https://www.kernel.org/doc/html/v4.14/crypto/api-samples.html#code-example-for-use-of-operational-state-memory-with-shash

static char auth_passwd_salt[32];
static struct crypto_shash *sha256_shash;
static char *auth_passwd;
extern char* activation_ct_passwd;

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
        return -ENOMEM;
    }

    memcpy(data_with_salt, data, datasize);
    memcpy(data_with_salt + datasize, auth_passwd_salt, saltsize);

    int rv = crypto_shash_digest(desc, data_with_salt, datasize + saltsize, output);

    kfree(desc);
    kfree(data_with_salt);

    return rv;
}

int password_cmp(const char* passwd) {
    if(sha256_shash != NULL) {
        char hashed_passwd[32];
        int rv = hash_sha256(passwd, strlen(passwd), hashed_passwd);
        if(rv < 0) {
            pr_err("%s: hash_sha256(...) failed, errno=%d\n", 
                module_name(THIS_MODULE), rv);
            return rv;
        }

        return memcmp(hashed_passwd, auth_passwd, 32);
    } else {
        return strcmp(passwd, auth_passwd);
    }
}

void destroy_passwd(void) {
    if(auth_passwd != NULL) {
        kfree(auth_passwd);
        auth_passwd = NULL;
    }

    if (sha256_shash != NULL) {
        crypto_free_shash(sha256_shash);
        sha256_shash = NULL;
    }
}

#ifndef CONFIG_SYSFS
#define write_cr0_forced(v) \
    do { \
        unsigned long val = (unsigned long) (v); \
        asm volatile("mov %0, %%cr0" : "+r"((val)) :: "memory"); \
    } while(0)
#endif

int setup_passwd(void) {
    size_t actpasswdlen = strlen(activation_ct_passwd);

    sha256_shash = crypto_alloc_shash("sha256", 0, 0);
    if (!IS_ERR(sha256_shash)) {
        auth_passwd = kmalloc(32 * sizeof(char), GFP_KERNEL);
        if(auth_passwd == NULL) {
            return -ENOMEM;
        }

        //https://elixir.bootlin.com/linux/v6.16/source/include/linux/random.h#L126
        //https://elixir.bootlin.com/linux/v6.16/source/drivers/char/random.c#L137
        //https://docs.kernel.org/6.6/kernel-hacking/hacking.html#user-context
        //setup_passwd() will be called only once, by the module_init func,
        //which is runned in user-context so it is ok to have a blocking
        //get_random_bytes_wait, to have good random numbers (crypto-secure)
        while(get_random_bytes_wait(auth_passwd_salt, 32) == -ERESTARTSYS)
            ;

        int rv = hash_sha256(activation_ct_passwd, actpasswdlen, auth_passwd);
        if(rv < 0) {
            pr_err("%s: hash_sha256(...) failed, errno=%d\n",
                   module_name(THIS_MODULE), rv);
            return rv;
        }
    } else {
        pr_warn("%s: fallback to ct passwd - crypto_alloc_shash(...) failed, errno=%ld\n", 
            module_name(THIS_MODULE), PTR_ERR(sha256_shash));

        sha256_shash = NULL;
        auth_passwd = kmalloc((actpasswdlen + 1) * sizeof(char), GFP_KERNEL);
        if(auth_passwd == NULL) {
            return -ENOMEM;
        }

        memcpy(auth_passwd, activation_ct_passwd, actpasswdlen * sizeof(char));
        auth_passwd[actpasswdlen] = 0;
    }

#ifndef CONFIG_SYSFS
    preempt_disable();
    unsigned long cr0 = read_cr0();
    write_cr0_forced(cr0 & ~X86_CR0_WP);
#endif

    memset(activation_ct_passwd, 0, actpasswdlen * sizeof(char));

#ifndef CONFIG_SYSFS
    write_cr0_forced(cr0);
    preempt_enable();
#endif

    return 0;
}

#ifndef CONFIG_SYSFS
#undef write_cr0_forced
#endif
