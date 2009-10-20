#include <unistd.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <sys/mount.h>
#include <sys/utsname.h>
#include <fcntl.h>
#include <stdlib.h>
#include <stdio.h>
#include <ctype.h>
#include <string.h>
#include <errno.h>
#include "selinux_internal.h"
#include <sepol/sepol.h>
#include <sepol/policydb.h>
#include <dlfcn.h>
#include "policy.h"
#include <limits.h>

int security_load_policy(void *data, size_t len)
{
	char path[PATH_MAX];
	int fd, ret;

	if (!selinux_mnt) {
		errno = ENOENT;
		return -1;
	}

	snprintf(path, sizeof path, "%s/load", selinux_mnt);
	fd = open(path, O_RDWR);
	if (fd < 0)
		return -1;

	ret = write(fd, data, len);
	close(fd);
	if (ret < 0)
		return -1;
	return 0;
}

hidden_def(security_load_policy)

int load_setlocaldefs hidden = 1;

#undef max
#define max(a, b) (((a) > (b)) ? (a) : (b))

int selinux_mkload_policy(int preservebools)
{	
	int kernvers = security_policyvers();
	int maxvers = kernvers, minvers = DEFAULT_POLICY_VERSION, vers;
	int setlocaldefs = load_setlocaldefs;
	char path[PATH_MAX];
	struct stat sb;
	struct utsname uts;
	size_t size;
	void *map, *data;
	int fd, rc = -1, prot;
	sepol_policydb_t *policydb;
	sepol_policy_file_t *pf;
	int usesepol = 0;
	int (*vers_max)(void) = NULL;
	int (*vers_min)(void) = NULL;
	int (*policy_file_create)(sepol_policy_file_t **) = NULL;
	void (*policy_file_free)(sepol_policy_file_t *) = NULL;
	void (*policy_file_set_mem)(sepol_policy_file_t *, char*, size_t) = NULL;
	int (*policydb_create)(sepol_policydb_t **) = NULL;
	void (*policydb_free)(sepol_policydb_t *) = NULL;
	int (*policydb_read)(sepol_policydb_t *, sepol_policy_file_t *) = NULL;
	int (*policydb_set_vers)(sepol_policydb_t *, unsigned int) = NULL;
	int (*policydb_to_image)(sepol_handle_t *, sepol_policydb_t *, void **, size_t *) = NULL;
	int (*genbools_array)(void *data, size_t len, char **names, int *values, int nel) = NULL;
	int (*genusers)(void *data, size_t len, const char *usersdir, void **newdata, size_t * newlen) = NULL;
	int (*genbools)(void *data, size_t len, char *boolpath) = NULL;

#ifdef SHARED
	char *errormsg = NULL;
	void *libsepolh = NULL;
	libsepolh = dlopen("libsepol.so.1", RTLD_NOW);
	if (libsepolh) {
		usesepol = 1;
		dlerror();
#define DLERR() if ((errormsg = dlerror())) goto dlclose;
		vers_max = dlsym(libsepolh, "sepol_policy_kern_vers_max");
		DLERR();
		vers_min = dlsym(libsepolh, "sepol_policy_kern_vers_min");
		DLERR();

		policy_file_create = dlsym(libsepolh, "sepol_policy_file_create");
		DLERR();
		policy_file_free = dlsym(libsepolh, "sepol_policy_file_free");
		DLERR();
		policy_file_set_mem = dlsym(libsepolh, "sepol_policy_file_set_mem");
		DLERR();
		policydb_create = dlsym(libsepolh, "sepol_policydb_create");
		DLERR();
		policydb_free = dlsym(libsepolh, "sepol_policydb_free");
		DLERR();
		policydb_read = dlsym(libsepolh, "sepol_policydb_read");
		DLERR();
		policydb_set_vers = dlsym(libsepolh, "sepol_policydb_set_vers");
		DLERR();
		policydb_to_image = dlsym(libsepolh, "sepol_policydb_to_image");
		DLERR();
		genbools_array = dlsym(libsepolh, "sepol_genbools_array");
		DLERR();
		genusers = dlsym(libsepolh, "sepol_genusers");
		DLERR();
		genbools = dlsym(libsepolh, "sepol_genbools");
		DLERR();

#undef DLERR
	}
#else
	usesepol = 1;
	vers_max = sepol_policy_kern_vers_max;
	vers_min = sepol_policy_kern_vers_min;
	policy_file_create = sepol_policy_file_create;
	policy_file_free = sepol_policy_file_free;
	policy_file_set_mem = sepol_policy_file_set_mem;
	policydb_create = sepol_policydb_create;
	policydb_free = sepol_policydb_free;
	policydb_read = sepol_policydb_read;
	policydb_set_vers = sepol_policydb_set_vers;
	policydb_to_image = sepol_policydb_to_image;
	genbools_array = sepol_genbools_array;
	genusers = sepol_genusers;
	genbools = sepol_genbools;

#endif

	/*
	 * Check whether we need to support local boolean and user definitions.
	 */
	if (setlocaldefs) {
		if (access(selinux_booleans_path(), F_OK) == 0)
			goto checkbool;
		snprintf(path, sizeof path, "%s.local", selinux_booleans_path());
		if (access(path, F_OK) == 0)
			goto checkbool;
		snprintf(path, sizeof path, "%s/local.users", selinux_users_path());
		if (access(path, F_OK) == 0)
			goto checkbool;
		/* No local definition files, so disable setlocaldefs. */
		setlocaldefs = 0;
	}

checkbool:
	/* 
	 * As of Linux 2.6.22, the kernel preserves boolean
	 * values across a reload, so we do not need to 
	 * preserve them in userspace.
	 */
	if (preservebools && uname(&uts) == 0 && strverscmp(uts.release, "2.6.22") >= 0)
		preservebools = 0;

	if (usesepol) {
		maxvers = vers_max();
		minvers = vers_min();
		if (!setlocaldefs && !preservebools)
			maxvers = max(kernvers, maxvers);
	}

	vers = maxvers;
      search:
	snprintf(path, sizeof(path), "%s.%d",
		 selinux_binary_policy_path(), vers);
	fd = open(path, O_RDONLY);
	while (fd < 0 && errno == ENOENT
	       && --vers >= minvers) {
		/* Check prior versions to see if old policy is available */
		snprintf(path, sizeof(path), "%s.%d",
			 selinux_binary_policy_path(), vers);
		fd = open(path, O_RDONLY);
	}
	if (fd < 0) {
		fprintf(stderr,
			"SELinux:  Could not open policy file <= %s.%d:  %s\n",
			selinux_binary_policy_path(), maxvers, strerror(errno));
		goto dlclose;
	}

	if (fstat(fd, &sb) < 0) {
		fprintf(stderr,
			"SELinux:  Could not stat policy file %s:  %s\n",
			path, strerror(errno));
		goto close;
	}

	prot = PROT_READ;
	if (setlocaldefs || preservebools)
		prot |= PROT_WRITE;

	size = sb.st_size;
	data = map = mmap(NULL, size, prot, MAP_PRIVATE, fd, 0);
	if (map == MAP_FAILED) {
		fprintf(stderr,
			"SELinux:  Could not map policy file %s:  %s\n",
			path, strerror(errno));
		goto close;
	}

	if (vers > kernvers && usesepol) {
		/* Need to downgrade to kernel-supported version. */
		if (policy_file_create(&pf))
			goto unmap;
		if (policydb_create(&policydb)) {
			policy_file_free(pf);
			goto unmap;
		}
		policy_file_set_mem(pf, data, size);
		if (policydb_read(policydb, pf)) {
			policy_file_free(pf);
			policydb_free(policydb);
			goto unmap;
		}
		if (policydb_set_vers(policydb, kernvers) ||
		    policydb_to_image(NULL, policydb, &data, &size)) {
			/* Downgrade failed, keep searching. */
			fprintf(stderr,
				"SELinux:  Could not downgrade policy file %s, searching for an older version.\n",
				path);
			policy_file_free(pf);
			policydb_free(policydb);
			munmap(map, sb.st_size);
			close(fd);
			vers--;
			goto search;
		}
		policy_file_free(pf);
		policydb_free(policydb);
	}

	if (usesepol) {
		if (setlocaldefs) {
			void *olddata = data;
			size_t oldsize = size;
			rc = genusers(olddata, oldsize, selinux_users_path(),
				      &data, &size);
			if (rc < 0) {
				/* Fall back to the prior image if genusers failed. */
				data = olddata;
				size = oldsize;
				rc = 0;
			} else {
				if (olddata != map)
					free(olddata);
			}
		}
		
#ifndef DISABLE_BOOL
		if (preservebools) {
			int *values, len, i;
			char **names;
			rc = security_get_boolean_names(&names, &len);
			if (!rc) {
				values = malloc(sizeof(int) * len);
				if (!values)
					goto unmap;
				for (i = 0; i < len; i++)
					values[i] =
						security_get_boolean_active(names[i]);
				(void)genbools_array(data, size, names, values,
						     len);
				free(values);
				for (i = 0; i < len; i++)
					free(names[i]);
				free(names);
			}
		} else if (setlocaldefs) {
			(void)genbools(data, size,
				       (char *)selinux_booleans_path());
		}
#endif
	}


	rc = security_load_policy(data, size);
	
	if (rc)
		fprintf(stderr,
			"SELinux:  Could not load policy file %s:  %s\n",
			path, strerror(errno));

      unmap:
	if (data != map)
		free(data);
	munmap(map, sb.st_size);
      close:
	close(fd);
      dlclose:
#ifdef SHARED
	if (errormsg)
		fprintf(stderr, "libselinux:  %s\n", errormsg);
	if (libsepolh)
		dlclose(libsepolh);
#endif
	return rc;
}

hidden_def(selinux_mkload_policy)

/*
 * Mount point for selinuxfs. 
 * This definition is private to the function below.
 * Everything else uses the location determined during 
 * libselinux startup via /proc/mounts (see init_selinuxmnt).  
 * We only need the hardcoded definition for the initial mount 
 * required for the initial policy load.
 */
int selinux_init_load_policy(int *enforce)
{
	int rc = 0, orig_enforce = 0, seconfig = -2, secmdline = -1;
	FILE *cfg;
	char *buf;

	/*
	 * Reread the selinux configuration in case it has changed.
	 * Example:  Caller has chroot'd and is now loading policy from
	 * chroot'd environment.
	 */
	selinux_reset_config();

	/*
	 * Get desired mode (disabled, permissive, enforcing) from 
	 * /etc/selinux/config. 
	 */
	selinux_getenforcemode(&seconfig);

	/* Check for an override of the mode via the kernel command line. */
	rc = mount("none", "/proc", "proc", 0, 0);
	cfg = fopen("/proc/cmdline", "r");
	if (cfg) {
		char *tmp;
		buf = malloc(selinux_page_size);
		if (!buf) {
			fclose(cfg);
			return -1;
		}
		if (fgets(buf, selinux_page_size, cfg) &&
		    (tmp = strstr(buf, "enforcing="))) {
			if (tmp == buf || isspace(*(tmp - 1))) {
				secmdline =
				    atoi(tmp + sizeof("enforcing=") - 1);
			}
		}
		fclose(cfg);
		free(buf);
	}
#ifndef MNT_DETACH
#define MNT_DETACH 2
#endif
	if (rc == 0)
		umount2("/proc", MNT_DETACH);

	/* 
	 * Determine the final desired mode.
	 * Command line argument takes precedence, then config file. 
	 */
	if (secmdline >= 0)
		*enforce = secmdline;
	else if (seconfig >= 0)
		*enforce = seconfig;
	else
		*enforce = 0;	/* unspecified or disabled */

	/*
	 * Check for the existence of SELinux via selinuxfs, and 
	 * mount it if present for use in the calls below.  
	 */
	if (mount("none", SELINUXMNT, "selinuxfs", 0, 0) < 0 && errno != EBUSY) {
		if (errno == ENODEV) {
			/*
			 * SELinux was disabled in the kernel, either
			 * omitted entirely or disabled at boot via selinux=0.
			 * This takes precedence over any config or
			 * commandline enforcing setting.
			 */
			*enforce = 0;
		} else {
			/* Only emit this error if selinux was not disabled */
			fprintf(stderr, "Mount failed for selinuxfs on %s:  %s\n", SELINUXMNT, strerror(errno));
		}
                
		goto noload;
	}
	set_selinuxmnt(SELINUXMNT);

	/*
	 * Note:  The following code depends on having selinuxfs 
	 * already mounted and selinuxmnt set above.
	 */

	if (seconfig == -1) {
		/* Runtime disable of SELinux. */
		rc = security_disable();
		if (rc == 0) {
			/* Successfully disabled, so umount selinuxfs too. */
			umount(SELINUXMNT);
		}
		/*
		 * If we failed to disable, SELinux will still be 
		 * effectively permissive, because no policy is loaded. 
		 * No need to call security_setenforce(0) here.
		 */
		goto noload;
	}

	/*
	 * If necessary, change the kernel enforcing status to match 
	 * the desired mode. 
	 */
	orig_enforce = rc = security_getenforce();
	if (rc < 0)
		goto noload;
	if (orig_enforce != *enforce) {
		rc = security_setenforce(*enforce);
		if (rc < 0) {
			fprintf(stderr, "SELinux:  Unable to switch to %s mode:  %s\n", (*enforce ? "enforcing" : "permissive"), strerror(errno));
			if (*enforce)
				goto noload;
		}
	}

	/* Load the policy. */
	return selinux_mkload_policy(0);

      noload:
	/*
	 * Only return 0 on a successful completion of policy load.
	 * In any other case, we want to return an error so that init
	 * knows not to proceed with the re-exec for the domain transition.
	 * Depending on the *enforce setting, init will halt (> 0) or proceed
	 * normally (otherwise).
	 */
	return -1;
}
