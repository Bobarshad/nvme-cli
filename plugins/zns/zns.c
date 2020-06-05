#include <fcntl.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <inttypes.h>
#include <linux/fs.h>
#include <sys/stat.h>

#include "nvme.h"

#define CREATE_CMD
#include "zns.h"

static int id_ctrl(int argc, char **argv, struct command *cmd, struct plugin *plugin)
{
	const char *desc = "Send ZNS specific Identify Controller command to "\
		"the given device and report information about the specified "\
		"controller in varios formats.";

	enum nvme_print_flags flags;
	struct nvme_zns_id_ctrl ctrl;
	int err, fd;

	struct config {
		char *output_format;
	};

	struct config cfg = {
		.output_format = "normal",
	};

	OPT_ARGS(opts) = {
		OPT_FMT("output-format", 'o', &cfg.output_format, output_format),
		OPT_END()
	};

	err = fd = parse_and_open(argc, argv, desc, opts);
	if (fd < 0)
		return errno;

	err = flags = validate_output_format(cfg.output_format);
	if (flags < 0)
		goto close_fd;

	err = nvme_zns_identify_ctrl(fd, &ctrl);
	if (!err)
		nvme_print_object(nvme_zns_id_ctrl_to_json(&ctrl, flags));
	else
		nvme_show_status("zns-id-ctrl", err);
close_fd:
	close(fd);
	return nvme_status_to_errno(err, false);
}

static int id_ns(int argc, char **argv, struct command *cmd, struct plugin *plugin)
{
	const char *desc = "Send ZNS specific Identify Namespace command to "\
		"the given device and report information about the specified "\
		"namespace in varios formats.";
	const char *namespace_id = "identifier of desired namespace";
	const char *verbose = "verbosely decode fields";

	enum nvme_print_flags flags;
	struct nvme_zns_id_ns ns;
	struct nvme_id_ns id_ns;
	int err, fd;

	struct config {
		char *output_format;
		int namespace_id;
		int verbose;
	};

	struct config cfg = {
		.output_format = "normal",
	};

	OPT_ARGS(opts) = {
		OPT_UINT("namespace-id", 'n', &cfg.namespace_id,  namespace_id),
		OPT_FMT("output-format", 'o', &cfg.output_format, output_format),
		OPT_FLAG("verbose",      'v', &cfg.verbose,       verbose),
		OPT_END()
	};

	err = fd = parse_and_open(argc, argv, desc, opts);
	if (fd < 0)
		return errno;

	err = flags = validate_output_format(cfg.output_format);
	if (flags < 0)
		goto close_fd;
	if (cfg.verbose)
		flags |= VERBOSE;

	if (!cfg.namespace_id) {
		cfg.namespace_id = nvme_get_nsid(fd);
		if (cfg.namespace_id <= 0) {
			if (!namespace_id) {
				errno = EINVAL;
				err = -1;
			} else
				err = cfg.namespace_id;
			fprintf(stderr, "Error: retrieving namespace-id\n");
			goto close_fd;
		}
	}

	err = nvme_identify_ns(fd, cfg.namespace_id, &id_ns);
	if (err) {
		nvme_show_status("id-ns", err);
		goto close_fd;
	}

	err = nvme_zns_identify_ns(fd, cfg.namespace_id, &ns);
	if (!err)
		nvme_print_object(nvme_zns_id_ns_to_json(&ns, &id_ns, flags));
	else
		nvme_show_status("zns-id-ns", err);
close_fd:
	close(fd);
	return nvme_status_to_errno(err, false);
}

static int __zns_mgmt_send(int fd, __u32 namespace_id, __u64 zslba,
	bool select_all, enum nvme_zns_send_action zsa, __u32 data_len, void *buf)
{
	int err;

	if (!namespace_id) {
		namespace_id = nvme_get_nsid(fd);
		if (namespace_id <= 0) {
			if (!namespace_id) {
				errno = EINVAL;
				err = -1;
			} else
				err = namespace_id;
			fprintf(stderr, "Error: retrieving namespace-id\n");
			goto close_fd;
		}
	}

	err = nvme_zns_mgmt_send(fd, namespace_id, zslba, select_all, zsa,
			data_len, buf);
close_fd:
	close(fd);
	return err;
}

static int zns_mgmt_send(int argc, char **argv, struct command *cmd, struct plugin *plugin,
	const char *desc, enum nvme_zns_send_action zsa)
{
	const char *zslba = "starting lba of the zone for this command";
	const char *namespace_id = "identifier of desired namespace";
	const char *select_all = "send command to all zones";

	int err, fd;
	char *command;

	struct config {
		__u64	zslba;
		int	namespace_id;
		bool	select_all;
	};

	struct config cfg = {
	};

	OPT_ARGS(opts) = {
		OPT_UINT("namespace-id", 'n', &cfg.namespace_id,  namespace_id),
		OPT_SUFFIX("start-lba",  's', &cfg.zslba,         zslba),
		OPT_FLAG("select-all",   'a', &cfg.select_all,    select_all),
		OPT_END()
	};

	err = asprintf(&command, "%s-%s", plugin->name, cmd->name);
	if (err < 0)
		return errno;

	err = fd = parse_and_open(argc, argv, desc, opts);
	if (fd < 0)
		goto free;

	err = __zns_mgmt_send(fd, cfg.namespace_id, cfg.zslba,
		cfg.select_all, zsa, 0, NULL);
	if (!err)
		printf("%s: Success, action:%d zone:%"PRIx64" nsid:%d\n", command,
			zsa, (uint64_t)zslba, cfg.namespace_id);
	else
		nvme_show_status(command, err);

free:
	free(command);
	return nvme_status_to_errno(err, false);
}

static int zone_mgmt_send(int argc, char **argv, struct command *cmd, struct plugin *plugin)
{
	const char *desc = "Zone Management Send";
	const char *zslba = "starting lba of the zone for this command";
	const char *namespace_id = "identifier of desired namespace";
	const char *select_all = "send command to all zones";
	const char *zsa = "zone send action";
	const char *data_len = "buffer length if data required";
	const char *data = "optional file for data (default stdin)";

	int err, fd, ffd = STDIN_FILENO;
	void *buf = NULL;

	struct config {
		__u64	zslba;
		int	namespace_id;
		bool	select_all;
		__u8	zsa;
		__u32   data_len;
		char   *file;
	};

	struct config cfg = {
	};

	OPT_ARGS(opts) = {
		OPT_UINT("namespace-id", 'n', &cfg.namespace_id,  namespace_id),
		OPT_SUFFIX("start-lba",  's', &cfg.zslba,         zslba),
		OPT_FLAG("select-all",   'a', &cfg.select_all,    select_all),
		OPT_BYTE("zsa",          'z', &cfg.zsa,           zsa),
		OPT_UINT("data-len",     'l', &cfg.data_len,     data_len),
		OPT_FILE("data",         'd', &cfg.file,         data),
		OPT_END()
	};

	err = fd = parse_and_open(argc, argv, desc, opts);
	if (fd < 0)
		return errno;

	if (cfg.data_len) {
		if (posix_memalign(&buf, getpagesize(), cfg.data_len)) {
			fprintf(stderr, "can not allocate feature payload\n");
			err = -1;
			goto close_fd;
		}
		memset(buf, 0, cfg.data_len);

		if (cfg.file) {
			ffd = open(cfg.file, O_RDONLY);
			if (ffd < 0) {
				perror(cfg.file);
				err = -1;
				goto free;
			}
		}

		err = read(ffd, (void *)buf, cfg.data_len);
		if (err < 0) {
			perror("read");
			err = -1;
			goto close_ffd;
		}
	}

	err = __zns_mgmt_send(fd, cfg.namespace_id, cfg.zslba, cfg.select_all,
			cfg.zsa, cfg.data_len, buf);
	if (!err)
		printf("zone-mgmt-send: Success, action:%d zone:%"PRIx64" nsid:%d\n",
			cfg.zsa, (uint64_t)cfg.zslba, cfg.namespace_id);
	else
		nvme_show_status("zone-mgmt-send", err);

close_ffd:
	if (cfg.file)
		close(ffd);
free:
	if (buf)
		free(buf);
close_fd:
	close(fd);
	return nvme_status_to_errno(err, false);
}

static int close_zone(int argc, char **argv, struct command *cmd, struct plugin *plugin)
{
	const char *desc = "Close zones\n";

	return zns_mgmt_send(argc, argv, cmd, plugin, desc, NVME_ZNS_ZSA_CLOSE);
}

static int finish_zone(int argc, char **argv, struct command *cmd, struct plugin *plugin)
{
	const char *desc = "Finish zones\n";

	return zns_mgmt_send(argc, argv, cmd, plugin, desc, NVME_ZNS_ZSA_FINISH);
}

static int open_zone(int argc, char **argv, struct command *cmd, struct plugin *plugin)
{
	const char *desc = "Open zones\n";

	return zns_mgmt_send(argc, argv, cmd, plugin, desc, NVME_ZNS_ZSA_OPEN);
}

static int reset_zone(int argc, char **argv, struct command *cmd, struct plugin *plugin)
{
	const char *desc = "Reset zones\n";

	return zns_mgmt_send(argc, argv, cmd, plugin, desc, NVME_ZNS_ZSA_RESET);
}

static int offline_zone(int argc, char **argv, struct command *cmd, struct plugin *plugin)
{
	const char *desc = "Offline zones\n";

	return zns_mgmt_send(argc, argv, cmd, plugin, desc, NVME_ZNS_ZSA_OFFLINE);
}

static int set_zone_desc(int argc, char **argv, struct command *cmd, struct plugin *plugin)
{
	const char *desc = "Set Zone Descriptor Extension\n";
	const char *zslba = "starting lba of the zone for this command";
	const char *namespace_id = "identifier of desired namespace";
	const char *data = "optional file for zone extention data (default stdin)";

	int err, fd, ffd = STDIN_FILENO;
	struct nvme_zns_id_ns ns;
	struct nvme_id_ns id_ns;
	void *buf = NULL;
	__u32 data_len;
	uint8_t lbaf;

	struct config {
		__u64	zslba;
		int	namespace_id;
		char   *file;
	};

	struct config cfg = {
	};

	OPT_ARGS(opts) = {
		OPT_UINT("namespace-id", 'n', &cfg.namespace_id,  namespace_id),
		OPT_SUFFIX("start-lba",  's', &cfg.zslba,         zslba),
		OPT_FILE("data",         'd', &cfg.file,         data),
		OPT_END()
	};

	err = fd = parse_and_open(argc, argv, desc, opts);
	if (fd < 0)
		return errno;

	err = nvme_identify_ns(fd, cfg.namespace_id, &id_ns);
	if (err) {
		nvme_show_status("id-ns", err);
		goto close_fd;
	}

	err = nvme_zns_identify_ns(fd, cfg.namespace_id, &ns);
	if (err) {
		nvme_show_status("zns-id-ns", err);
		goto close_fd;
	}

	lbaf = id_ns.flbas & NVME_NS_FLBAS_LBA_MASK;
	data_len = ns.lbafe[lbaf].zdes;

	if (!data_len) {
		fprintf(stderr,
			"zone format does not provide descriptor extention\n");
		errno = EINVAL;
		err = -1;
		goto close_fd;
	}

	buf = malloc(data_len);
	if (!buf) {
		err = -1;
		goto close_fd;
	}

	if (cfg.file) {
		ffd = open(cfg.file, O_RDONLY);
		if (ffd < 0) {
			perror(cfg.file);
			err = -1;
			goto free;
		}
	}

	err = read(ffd, (void *)buf, data_len);
	if (err < 0) {
		perror("read");
		err = -1;
		goto close_ffd;
	}

	err = __zns_mgmt_send(fd, cfg.namespace_id, cfg.zslba, 0,
		NVME_ZNS_ZSA_SET_DESC_EXT, data_len, buf);
	if (!err)
		printf("set-zone-desc: Success, zone:%"PRIx64" nsid:%d\n",
			(uint64_t)cfg.zslba, cfg.namespace_id);
	else
		nvme_show_status("set-zone-desc", err);
close_ffd:
	if (cfg.file)
		close(ffd);
free:
	free(buf);
close_fd:
	close(fd);
	return nvme_status_to_errno(err, false);
}

static int zone_mgmt_recv(int argc, char **argv, struct command *cmd, struct plugin *plugin)
{
	return 0;
}

static int get_zdes(int fd, __u32 nsid)
{
	struct nvme_zns_id_ns ns;
	struct nvme_id_ns id_ns;
	__u8 lbaf;
	int err;

	err = nvme_identify_ns(fd, nsid,  &id_ns);
	if (err) {
		nvme_show_status("id-ns", err);
		return err;
	}

	err = nvme_zns_identify_ns(fd, nsid,  &ns);
	if (err) {
		nvme_show_status("zns-id-ns", err);
		return err;
	}

	lbaf = id_ns.flbas & NVME_NS_FLBAS_LBA_MASK;
	return ns.lbafe[lbaf].zdes;
}

static int report_zones(int argc, char **argv, struct command *cmd, struct plugin *plugin)
{
	const char *desc = "Retrieve the Report Zones data structure";
	const char *zslba = "starting lba of the zone";
	const char *num_descs = "number of descriptors to retrieve";
	const char *state = "state of zones to list";
	const char *ext = "set to use the extended report zones";
	const char *part = "set to use the partial report";
	const char *verbose = "verbosely decode fields";
	const char *namespace_id = "identifier of desired namespace";
	
	enum nvme_print_flags flags;
	int err, fd, zdes = 0;
	__u32 report_size;
	void *report;
	bool huge = false;

	struct config {
		char *output_format;
		__u64 zslba;
		int   namespace_id;
		int   num_descs;
		int   state;
		bool  verbose;
		bool  extended;
		bool  partial;
	};
	
	struct config cfg = {
		.output_format = "normal",
	};

	OPT_ARGS(opts) = {
		OPT_UINT("namespace-id", 'n', &cfg.namespace_id,  namespace_id),
		OPT_SUFFIX("start-lba",  's', &cfg.zslba,         zslba),
		OPT_UINT("descs",        'd', &cfg.num_descs,  num_descs),
		OPT_UINT("state",         'S', &cfg.state,         state),
		OPT_FMT("output-format", 'o', &cfg.output_format, output_format),
		OPT_FLAG("verbose",      'v', &cfg.verbose,       verbose),
		OPT_FLAG("extended",     'e', &cfg.extended,      ext),
		OPT_FLAG("partial",      'p', &cfg.partial,       part),
		OPT_END()
	};

	err = fd = parse_and_open(argc, argv, desc, opts);
	if (fd < 0)
		return errno;

	err = flags = validate_output_format(cfg.output_format);
	if (flags < 0)
		goto close_fd;
	if (cfg.verbose)
		flags |= VERBOSE;

	if (!cfg.namespace_id) {
		cfg.namespace_id = nvme_get_nsid(fd);
		if (cfg.namespace_id <= 0) {
			if (!namespace_id) {
				errno = EINVAL;
				err = -1;
			} else
				err = cfg.namespace_id;
			fprintf(stderr, "Error: retrieving namespace-id\n");
			goto close_fd;
		}
	}

	if (cfg.extended) {
		zdes = get_zdes(fd, cfg.namespace_id);
		if (zdes < 0) {
			err = zdes;
			goto close_fd;
		}
	}

	report_size = sizeof(struct nvme_zone_report) + cfg.num_descs *
		(sizeof(struct nvme_zns_desc) + zdes);

	report = nvme_alloc(report_size, &huge);
	if (!report) {
		perror("malloc");
		err = -1;
		goto close_fd;
	}

	err = nvme_zns_report_zones(fd, cfg.namespace_id, cfg.zslba,
		cfg.extended, cfg.state, cfg.partial, report_size, report);
	if (!err)
		nvme_print_object(nvme_zns_report_zones_to_json(report,
			cfg.num_descs, zdes, report_size, flags));
	else
		nvme_show_status("report-zones", err);

	nvme_free(report, huge);
close_fd:
	close(fd);
	return nvme_status_to_errno(err, false);
}

static int zone_append(int argc, char **argv, struct command *cmd, struct plugin *plugin)
{
	const char *desc = "The zone append command is used to write to a zone "\
		  "using the slba of the zone, and the write will be appended from the "\
		  "write pointer of the zone";
	const char *zslba = "starting lba of the zone";
	const char *data = "file containing data to write";
	const char *metadata = "file with metadata to be written";
	const char *limited_retry = "limit media access attempts";
	const char *fua = "force unit access";
	const char *prinfo = "protection information action and checks field";
	const char *ref_tag = "reference tag (for end to end PI)";
	const char *lbat = "logical block application tag (for end to end PI)";
	const char *lbatm = "logical block application tag mask (for end to end PI)";
	const char *metadata_size = "size of metadata in bytes";
	const char *data_size = "size of data in bytes";

	int err = 0, fd, dfd = STDIN_FILENO, mfd = STDIN_FILENO;
	unsigned int lba_size, meta_size;
	void *buf = NULL, *mbuf = NULL;
	__u16 nblocks, control = 0;
	__u64 result;

	nvme_ns_t ns;

	struct config {
		char  *data;
		char  *metadata;
		__u64  zslba;
		__u64  data_size;
		__u64  metadata_size;
		int    limited_retry;
		int    fua;
		__u32  ref_tag;
		__u16  lbat;
		__u16  lbatm;
		__u8   prinfo;
	};

	struct config cfg = {
	};

	OPT_ARGS(opts) = {
	        OPT_SUFFIX("zslba",           's', &cfg.zslba,         zslba),
	        OPT_SUFFIX("data-size",       'z', &cfg.data_size,     data_size),
	        OPT_SUFFIX("metadata-size",   'y', &cfg.metadata_size, metadata_size),
		OPT_FILE("data",              'd', &cfg.data,          data),
		OPT_FILE("metadata",          'M', &cfg.metadata,      metadata),
		OPT_FLAG("limited-retry",     'l', &cfg.limited_retry, limited_retry),
		OPT_FLAG("force-unit-access", 'f', &cfg.fua,           fua),
		OPT_UINT("ref-tag",           'r', &cfg.ref_tag,       ref_tag),
		OPT_SHRT("app-tag-mask",      'm', &cfg.lbatm,         lbatm),
		OPT_SHRT("app-tag",           'a', &cfg.lbat,          lbat),
		OPT_BYTE("prinfo",            'p', &cfg.prinfo,        prinfo),
		OPT_END()
	};

	err = fd = parse_and_open(argc, argv, desc, opts);
	if (fd < 0)
	        return errno;

	if (!cfg.data_size) {
		fprintf(stderr, "Append size not provided\n");
		errno = EINVAL;
		err = -1;
		goto close_fd;
	}

	ns = nvme_scan_namespace(devicename);
	if (!ns) {
		fprintf(stderr, "Failed to open requested namespace:%s\n",
			devicename);
		errno = EINVAL;
		err = -1;
		goto close_fd;
	}

	lba_size = nvme_ns_get_lba_size(ns);
	if (cfg.data_size & (lba_size - 1)) {
		fprintf(stderr,
			"Data size:%#"PRIx64" not aligned to lba size:%#x\n",
			(uint64_t)cfg.data_size, lba_size);
		errno = EINVAL;
		err = -1;
		goto close_ns;
	}

	meta_size = nvme_ns_get_meta_size(ns);
	if (meta_size && (!cfg.metadata_size || cfg.metadata_size % meta_size)) {
		fprintf(stderr,
			"Metadata size:%#"PRIx64" not aligned to metadata size:%#x\n",
			(uint64_t)cfg.metadata_size, meta_size);
		errno = EINVAL;
		err = -1;
		goto close_ns;
	}

	if (cfg.prinfo > 0xf) {
	        fprintf(stderr, "Invalid value for prinfo:%#x\n", cfg.prinfo);
		errno = EINVAL;
		err = -1;
		goto close_ns;
	}

	if (cfg.data) {
		dfd = open(cfg.data, O_RDONLY);
		if (dfd < 0) {
			perror(cfg.data);
			err = -1;
			goto close_ns;
		}
	}

	if (posix_memalign(&buf, getpagesize(), cfg.data_size)) {
		fprintf(stderr, "No memory for data size:%"PRIx64"\n",
			(uint64_t)cfg.data_size);
		err = -1;
		goto close_dfd;
	}

	memset(buf, 0, cfg.data_size);
	err = read(dfd, buf, cfg.data_size);
	if (err < 0) {
		perror("read-data");
		goto free_data;
	}

	if (cfg.metadata) {
		mfd = open(cfg.metadata, O_RDONLY);
		if (mfd < 0) {
			perror(cfg.metadata);
			err = -1;
			goto close_dfd;
		}
	}

	if (cfg.metadata_size) {
		if (posix_memalign(&mbuf, getpagesize(), meta_size)) {
			fprintf(stderr, "No memory for metadata size:%d\n",
				meta_size);
			err = -1;
			goto close_mfd;
		}

		memset(mbuf, 0, cfg.metadata_size);
		err = read(mfd, mbuf, cfg.metadata_size);
		if (err < 0) {
			perror("read-metadata");
			goto free_meta;
		}
	}

	nblocks = (cfg.data_size / lba_size) - 1;
	control |= (cfg.prinfo << 10);
	if (cfg.limited_retry)
		control |= NVME_IO_LR;
	if (cfg.fua)
		control |= NVME_IO_FUA;

	printf("sending zone append to %s namespace %d\n", devicename,
		nvme_ns_get_nsid(ns));
	err = nvme_zns_append(fd, nvme_ns_get_nsid(ns), cfg.zslba, nblocks,
			      control, cfg.ref_tag, cfg.lbat, cfg.lbatm,
			      cfg.data_size, buf, cfg.metadata_size, mbuf,
			      &result);
	if (!err)
		printf("Success appended data to LBA %"PRIx64"\n", (uint64_t)result);
	else
		nvme_show_status("zone-append", err);

free_meta:
	free(mbuf);
close_mfd:
	if (cfg.metadata)
		close(mfd);
free_data:
	free(buf);
close_dfd:
	if (cfg.data)
		close(dfd);
close_ns:
	nvme_free_ns(ns);
close_fd:
	close(fd);
	return err;
}

static int change_zone_list(int argc, char **argv, struct command *cmd, struct plugin *plugin)
{
	return 0;
}
