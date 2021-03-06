#include "cache.h"
#include "config.h"
#include "run-command.h"
#include "strbuf.h"
#include "gpg-interface.h"
#include "sigchain.h"
#include "tempfile.h"
#include "signing-interface.h"

#if 0

/*static char *configured_signing_key;*/
struct gpg_format {
	const char *name;
	const char *program;
	const char **verify_args;
	const char **sigs;
};

static const char *openpgp_verify_args[] = {
	"--keyid-format=long",
	NULL
};
static const char *openpgp_sigs[] = {
	"-----BEGIN PGP SIGNATURE-----",
	"-----BEGIN PGP MESSAGE-----",
	NULL
};

static const char *x509_verify_args[] = {
	NULL
};
static const char *x509_sigs[] = {
	"-----BEGIN SIGNED MESSAGE-----",
	NULL
};

static struct gpg_format gpg_format[] = {
	{ .name = "openpgp", .program = "gpg",
	  .verify_args = openpgp_verify_args,
	  .sigs = openpgp_sigs
	},
	{ .name = "x509", .program = "gpgsm",
	  .verify_args = x509_verify_args,
	  .sigs = x509_sigs
	},
};


static struct gpg_format *use_format = &gpg_format[0];

static struct gpg_format *get_format_by_name(const char *str)
{
	int i;

	for (i = 0; i < ARRAY_SIZE(gpg_format); i++)
		if (!strcmp(gpg_format[i].name, str))
			return gpg_format + i;
	return NULL;
}


static struct gpg_format *get_format_by_sig(const char *sig)
{
	int i, j;

	for (i = 0; i < ARRAY_SIZE(gpg_format); i++)
		for (j = 0; gpg_format[i].sigs[j]; j++)
			if (starts_with(sig, gpg_format[i].sigs[j]))
				return gpg_format + i;
	return NULL;
}
*/

void signature_check_clear(struct signature *sigc)
{
	FREE_AND_NULL(sigc->sig.buf);
	FREE_AND_NULL(sigc->output.buf);
	FREE_AND_NULL(sigc->status.buf);
	FREE_AND_NULL(sigc->signer);
	FREE_AND_NULL(sigc->key);
	FREE_AND_NULL(sigc->fingerprint);
	FREE_AND_NULL(sigc->primary_key_fingerprint);
}
*/

/* An exclusive status -- only one of them can appear in output */
#define GPG_STATUS_EXCLUSIVE	(1<<0)
/* The status includes key identifier */
#define GPG_STATUS_KEYID	(1<<1)
/* The status includes user identifier */
#define GPG_STATUS_UID		(1<<2)
/* The status includes key fingerprints */
#define GPG_STATUS_FINGERPRINT	(1<<3)

/* Short-hand for standard exclusive *SIG status with keyid & UID */
#define GPG_STATUS_STDSIG	(GPG_STATUS_EXCLUSIVE|GPG_STATUS_KEYID|GPG_STATUS_UID)

static struct {
	char result;
	const char *check;
	unsigned int flags;
} sigcheck_gpg_status[] = {
	{ 'G', "GOODSIG ", GPG_STATUS_STDSIG },
	{ 'B', "BADSIG ", GPG_STATUS_STDSIG },
	{ 'U', "TRUST_NEVER", 0 },
	{ 'U', "TRUST_UNDEFINED", 0 },
	{ 'E', "ERRSIG ", GPG_STATUS_EXCLUSIVE|GPG_STATUS_KEYID },
	{ 'X', "EXPSIG ", GPG_STATUS_STDSIG },
	{ 'Y', "EXPKEYSIG ", GPG_STATUS_STDSIG },
	{ 'R', "REVKEYSIG ", GPG_STATUS_STDSIG },
	{ 0, "VALIDSIG ", GPG_STATUS_FINGERPRINT },
};

static void parse_gpg_output(struct signature *sigc)
{
	const char *buf = sigc->status.buf;
	const char *line, *next;
	int i, j;
	int seen_exclusive_status = 0;

	/* Iterate over all lines */
	for (line = buf; *line; line = strchrnul(line+1, '\n')) {
		while (*line == '\n')
			line++;
		/* Skip lines that don't start with GNUPG status */
		if (!skip_prefix(line, "[GNUPG:] ", &line))
			continue;

		/* Iterate over all search strings */
		for (i = 0; i < ARRAY_SIZE(sigcheck_gpg_status); i++) {
			if (skip_prefix(line, sigcheck_gpg_status[i].check, &line)) {
				if (sigcheck_gpg_status[i].flags & GPG_STATUS_EXCLUSIVE) {
					if (seen_exclusive_status++)
						goto found_duplicate_status;
				}

				if (sigcheck_gpg_status[i].result)
					sigc->result = sigcheck_gpg_status[i].result;
				/* Do we have key information? */
				if (sigcheck_gpg_status[i].flags & GPG_STATUS_KEYID) {
					next = strchrnul(line, ' ');
					free(sigc->key);
					sigc->key = xmemdupz(line, next - line);
					/* Do we have signer information? */
					if (*next && (sigcheck_gpg_status[i].flags & GPG_STATUS_UID)) {
						line = next + 1;
						next = strchrnul(line, '\n');
						free(sigc->signer);
						sigc->signer = xmemdupz(line, next - line);
					}
				}
				/* Do we have fingerprint? */
				if (sigcheck_gpg_status[i].flags & GPG_STATUS_FINGERPRINT) {
					next = strchrnul(line, ' ');
					free(sigc->fingerprint);
					sigc->fingerprint = xmemdupz(line, next - line);

					/* Skip interim fields */
					for (j = 9; j > 0; j--) {
						if (!*next)
							break;
						line = next + 1;
						next = strchrnul(line, ' ');
					}

					next = strchrnul(line, '\n');
					free(sigc->primary_key_fingerprint);
					sigc->primary_key_fingerprint = xmemdupz(line, next - line);
				}

				break;
			}
		}
	}
	return;

found_duplicate_status:
	/*
	 * GOODSIG, BADSIG etc. can occur only once for each signature.
	 * Therefore, if we had more than one then we're dealing with multiple
	 * signatures.  We don't support them currently, and they're rather
	 * hard to create, so something is likely fishy and we should reject
	 * them altogether.
	 */
	sigc->result = 'E';
	/* Clear partial data to avoid confusion */
	FREE_AND_NULL(sigc->primary_key_fingerprint);
	FREE_AND_NULL(sigc->fingerprint);
	FREE_AND_NULL(sigc->signer);
	FREE_AND_NULL(sigc->key);
}

int check_signature(const char *payload, size_t plen, const char *signature,
	size_t slen, struct signature *sigc)
{
	struct strbuf gpg_output = STRBUF_INIT;
	struct strbuf gpg_status = STRBUF_INIT;
	int status;

	sigc->result = 'N';

	status = verify_signed_buffer(payload, plen, signature, slen,
				      &gpg_output, &gpg_status);
	if (status && !gpg_output.len)
		goto out;
	sigc->sig.buf = xmemdupz(payload, plen);
	sigc->output.buf = strbuf_detach(&gpg_output, NULL);
	sigc->status.buf = strbuf_detach(&gpg_status, NULL);
	parse_gpg_output(sigc);
	status |= sigc->result != 'G' && sigc->result != 'U';

 out:
	strbuf_release(&gpg_status);
	strbuf_release(&gpg_output);

	return !!status;
}

void print_signature_buffer(const struct signature *sigc, unsigned flags)
{
	const char *output = flags & OUTPUT_RAW ?
		sigc->status.buf : sigc->output.buf;

	if (flags & OUTPUT_VERBOSE && sigc->sig.buf)
		fputs(sigc->sig.buf, stdout);

	if (output)
		fputs(output, stderr);
}


size_t parse_signature(const char *buf, size_t size)
{
	size_t len = 0;
	size_t match = size;
	while (len < size) {
		const char *eol;

		if (get_format_by_sig(buf + len))
			match = len;

		eol = memchr(buf + len, '\n', size - len);
		len += eol ? eol - (buf + len) + 1 : size - len;
	}
	return match;
}


static FILE *thelog = NULL;
static int indent = 0;
static int dolog = 0;
struct strbuf strlog = STRBUF_INIT;
static const char *logpath = "/home/user/gpg.log";

#define IN(...) { \
	strbuf_addchars(&strlog, ' ', indent * 2); \
	strbuf_addf(&strlog, __VA_ARGS__); \
	indent++; \
	if(thelog == NULL) { \
		thelog = fopen(logpath, "a"); \
	} \
} while(0)

#define OUT(...) { \
	indent--; \
	strbuf_addchars(&strlog, ' ', indent * 2); \
	strbuf_addf(&strlog, __VA_ARGS__); \
} while(0)

#define OFF { \
	if(thelog != NULL) { \
		if(dolog) { \
			strbuf_write(&strlog, thelog); \
			strbuf_release(&strlog); \
		} \
		fclose(thelog); \
		thelog = NULL; \
	} \
	dolog = 0; \
} while(0)

#define LOG(...) { \
	strbuf_addchars(&strlog, ' ', indent * 2); \
	strbuf_addf(&strlog, __VA_ARGS__); \
	dolog = 1; \
} while(0)

void set_signing_key(const char *key)
{
	IN("set_signing_key(%s) {\n", key);
	free(configured_signing_key);
	configured_signing_key = xstrdup(key);
	LOG("ok: signing_key = %s\n", configured_signing_key);
	OUT("}\n");
}

int git_gpg_config(const char *var, const char *value, void *cb)
{
	struct gpg_format *fmt = NULL;
	char *fmtname = NULL;

	IN("git_gpg_config(%s, %s, %p) {\n", var, value, cb);

	if (!strcmp(var, "user.signingkey")) {
		if (!value) {
			LOG("error: setting user.signingkey without value\n");
			OUT("}\n");
			OFF;
			return config_error_nonbool(var);
		}
		set_signing_key(value);
		OUT("}\n");
		OFF;
		return 0;
	}

	if (!strcmp(var, "gpg.format")) {
		if (!value) {
			LOG("error: setting gpg.format without value\n");
			OUT("}\n");
			OFF;
			return config_error_nonbool(var);
		}
		fmt = get_format_by_name(value);
		if (!fmt) {
			LOG("error: no format for %s\n", value);
			OUT("}\n");
			OFF;
			return error("unsupported value for %s: %s",
				     var, value);
		}
		LOG("ok: format = %s via gpg.format\n", fmt->name);
		use_format = fmt;
		OUT("}\n");
		OFF;
		return 0;
	}

	if (!strcmp(var, "gpg.program") || !strcmp(var, "gpg.openpgp.program")) {
		LOG("ok: format lookup by name 'openpgp' via %s\n", var);
		fmtname = "openpgp";
	}

	if (!strcmp(var, "gpg.x509.program")) {
		LOG("ok: format lookup by name 'x509' via %s\n", var);
		fmtname = "x509";
	}

	if (fmtname) {
		fmt = get_format_by_name(fmtname);
		LOG("ok: looking up git_config_string(%s, %s, %p)\n", fmt->program, var, value);
		OUT("}\n");
		OFF;
		return git_config_string(&fmt->program, var, value);
	}

	OUT("}\n");
	OFF;
	return 0;
}

const char *get_signing_key(void)
{
	const char *r = NULL;
	IN("get_signing_key() {\n");
	if (configured_signing_key) {
		LOG("ok: signing key: %s\n", configured_signing_key);
		OUT("}\n");
		return configured_signing_key;
	}
	r = git_committer_info(IDENT_STRICT|IDENT_NO_DATE);
	LOG("ok: signing key: %s\n", r);
	OUT("}\n");
	return r;
}


int sign_buffer(struct strbuf *buffer, struct strbuf *signature, const char *signing_key)
{
	/* Update gpg program */
	extern enum signature_type default_type;
	const char *signing_program = xstrdup(get_signing_program(default_type));

	/* For debugging to be removed*/
	//printf("Program: %s\n", s_program);
	//printf("Signing key: %s\n", signing_key);
	
	struct child_process gpg = CHILD_PROCESS_INIT;
	int ret;
	size_t i, j, bottom;
	struct strbuf gpg_status = STRBUF_INIT;

	argv_array_pushl(&gpg.args,
			 signing_program,
			 "--status-fd=2",
			 "-bsau", signing_key,
			 NULL);

	bottom = signature->len;

	/*
	 * When the username signingkey is bad, program could be terminated
	 * because gpg exits without reading and then write gets SIGPIPE.
	 */
	sigchain_push(SIGPIPE, SIG_IGN);
	ret = pipe_command(&gpg, buffer->buf, buffer->len,
			   signature, 1024, &gpg_status, 0);
	sigchain_pop(SIGPIPE);

	/* For debugging to be removed */
	//printf("Output:\n %s\n", gpg_status.buf);

	ret |= !strstr(gpg_status.buf, "\n[GNUPG:] SIG_CREATED ");
	strbuf_release(&gpg_status);
	if (ret)
		return error(_("gpg failed to sign the data"));

	/* Strip CR from the line endings, in case we are on Windows. */
	for (i = j = bottom; i < signature->len; i++)
		if (signature->buf[i] != '\r') {
			if (i != j)
				signature->buf[j] = signature->buf[i];
			j++;
		}
	strbuf_setlen(signature, j);

	return 0;
}


int verify_signed_buffer(const char *payload, size_t payload_size,
			 const char *signature, size_t signature_size,
			 struct strbuf *gpg_output, struct strbuf *gpg_status)
{
	struct child_process gpg = CHILD_PROCESS_INIT;
	struct gpg_format *fmt;
	struct tempfile *temp;
	int ret;
	struct strbuf buf = STRBUF_INIT;

	temp = mks_tempfile_t(".git_vtag_tmpXXXXXX");
	if (!temp)
		return error_errno(_("could not create temporary file"));
	if (write_in_full(temp->fd, signature, signature_size) < 0 ||
	    close_tempfile_gently(temp) < 0) {
		error_errno(_("failed writing detached signature to '%s'"),
			    temp->filename.buf);
		delete_tempfile(&temp);
		return -1;
	}

	fmt = get_format_by_sig(signature);
	if (!fmt)
		BUG("bad signature '%s'", signature);

	argv_array_push(&gpg.args, fmt->program);
	argv_array_pushv(&gpg.args, fmt->verify_args);
	argv_array_pushl(&gpg.args,
			 "--status-fd=1",
			 "--verify", temp->filename.buf, "-",
			 NULL);

	if (!gpg_status)
		gpg_status = &buf;

	sigchain_push(SIGPIPE, SIG_IGN);
	ret = pipe_command(&gpg, payload, payload_size,
			   gpg_status, 0, gpg_output, 0);
	sigchain_pop(SIGPIPE);

	delete_tempfile(&temp);

	ret |= !strstr(gpg_status->buf, "\n[GNUPG:] GOODSIG ");
	strbuf_release(&buf); /* no matter it was used or not */

	return ret;
}
#endif