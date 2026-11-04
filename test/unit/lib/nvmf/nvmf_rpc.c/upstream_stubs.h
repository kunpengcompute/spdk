/* SPDX-License-Identifier: BSD-3-Clause */

/*Stubs for the unchanged upstream RPC handlers registered by nvmf_rpc.c. */
DEFINE_STUB(nvmf_subsystem_find_listener, struct spdk_nvmf_subsystem_listener *,
	(struct spdk_nvmf_subsystem *subsystem, const struct spdk_nvme_transport_id *trid), NULL);
DEFINE_STUB(nvmf_subsystem_get_ana_reporting, bool, (struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB_V(nvmf_subsystem_remove_all_listeners,
	(struct spdk_nvmf_subsystem *subsystem, bool stop));
DEFINE_STUB_V(nvmf_subsystem_set_ana_state,
	(struct spdk_nvmf_subsystem *subsystem, const struct spdk_nvme_transport_id *trid,
		enum spdk_nvme_ana_state ana_state, uint32_t anagrpid,
		spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn, void *cb_arg));
DEFINE_STUB(nvmf_subsystem_set_cntlid_range, int,
	(struct spdk_nvmf_subsystem *subsystem, uint16_t min_cntlid, uint16_t max_cntlid), 0);
DEFINE_STUB_V(nvmf_transport_dump_opts,
	(struct spdk_nvmf_transport *transport, struct spdk_json_write_ctx *w, bool named));
DEFINE_STUB_V(nvmf_transport_listen_dump_opts,
	(struct spdk_nvmf_transport *transport, const struct spdk_nvme_transport_id *trid,
		struct spdk_json_write_ctx *w));
DEFINE_STUB(spdk_bdev_get_name, const char *, (const struct spdk_bdev *bdev), NULL);
DEFINE_STUB(spdk_bit_array_count_set, uint32_t, (const struct spdk_bit_array *ba), 0);
DEFINE_STUB(spdk_io_channel_iter_get_channel, struct spdk_io_channel *,
	(struct spdk_io_channel_iter *i), NULL);
DEFINE_STUB_V(spdk_jsonrpc_send_error_response_fmt,
	(struct spdk_jsonrpc_request *request, int error_code, const char *fmt, ...));
DEFINE_STUB(spdk_nvme_transport_id_adrfam_str, const char *, (enum spdk_nvmf_adrfam adrfam), NULL);
DEFINE_STUB(spdk_nvme_transport_id_parse_adrfam, int,
	(enum spdk_nvmf_adrfam *adrfam, const char *str), 0);
DEFINE_STUB(spdk_nvme_transport_id_parse_trtype, int,
	(enum spdk_nvme_transport_type *trtype, const char *str), 0);
DEFINE_STUB(spdk_nvme_transport_id_populate_trstring, int,
	(struct spdk_nvme_transport_id *trid, const char *trstring), 0);
DEFINE_STUB(spdk_nvmf_get_first_tgt, struct spdk_nvmf_tgt *, (void), NULL);
DEFINE_STUB(spdk_nvmf_get_next_tgt, struct spdk_nvmf_tgt *, (struct spdk_nvmf_tgt *prev), NULL);
DEFINE_STUB(spdk_nvmf_host_get_nqn, const char *, (const struct spdk_nvmf_host *host), NULL);
DEFINE_STUB_V(spdk_nvmf_listen_opts_init, (struct spdk_nvmf_listen_opts *opts, size_t opts_size));
DEFINE_STUB(spdk_nvmf_ns_get_bdev, struct spdk_bdev *, (struct spdk_nvmf_ns *ns), NULL);
DEFINE_STUB(spdk_nvmf_ns_get_id, uint32_t, (const struct spdk_nvmf_ns *ns), 0);
DEFINE_STUB_V(spdk_nvmf_ns_get_opts,
	(const struct spdk_nvmf_ns *ns, struct spdk_nvmf_ns_opts *opts, size_t opts_size));
DEFINE_STUB_V(spdk_nvmf_ns_opts_get_defaults, (struct spdk_nvmf_ns_opts *opts, size_t opts_size));
DEFINE_STUB_V(spdk_nvmf_poll_group_dump_stat,
	(struct spdk_nvmf_poll_group *group, struct spdk_json_write_ctx *w));
DEFINE_STUB(spdk_nvmf_qpair_get_listen_trid, int,
	(struct spdk_nvmf_qpair *qpair, struct spdk_nvme_transport_id *trid), 0);
DEFINE_STUB(spdk_nvmf_subsystem_add_host, int,
	(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn), 0);
DEFINE_STUB_V(spdk_nvmf_subsystem_add_listener,
	(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvme_transport_id *trid,
		spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn, void *cb_arg));
DEFINE_STUB(spdk_nvmf_subsystem_add_ns_ext, uint32_t,
	(struct spdk_nvmf_subsystem *subsystem, const char *bdev_name,
		const struct spdk_nvmf_ns_opts *opts, size_t opts_size, const char *ptpl_file), 0);
DEFINE_STUB(spdk_nvmf_subsystem_create, struct spdk_nvmf_subsystem *,
	(struct spdk_nvmf_tgt *tgt, const char *nqn, enum spdk_nvmf_subtype type, uint32_t num_ns), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_destroy, int,
	(struct spdk_nvmf_subsystem *subsystem, nvmf_subsystem_destroy_cb cpl_cb, void *cpl_cb_arg), 0);
DEFINE_STUB(spdk_nvmf_subsystem_disconnect_host, int,
	(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn,
		spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_allow_any_host, bool,
	(const struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_first, struct spdk_nvmf_subsystem *,
	(struct spdk_nvmf_tgt *tgt), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_first_host, struct spdk_nvmf_host *,
	(struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_first_listener, struct spdk_nvmf_subsystem_listener *,
	(struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_first_ns, struct spdk_nvmf_ns *,
	(struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_max_cntlid, uint16_t,
	(const struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_max_namespaces, uint32_t,
	(const struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_min_cntlid, uint16_t,
	(const struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_subsystem_get_mn, const char *,
	(const struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_next, struct spdk_nvmf_subsystem *,
	(struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_next_host, struct spdk_nvmf_host *,
	(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_host *prev_host), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_next_listener, struct spdk_nvmf_subsystem_listener *,
	(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_subsystem_listener *prev_listener), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_next_ns, struct spdk_nvmf_ns *,
	(struct spdk_nvmf_subsystem *subsystem, struct spdk_nvmf_ns *prev_ns), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_nqn, const char *,
	(const struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_sn, const char *,
	(const struct spdk_nvmf_subsystem *subsystem), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_get_type, enum spdk_nvmf_subtype,
	(struct spdk_nvmf_subsystem *subsystem), 0);
DEFINE_STUB(spdk_nvmf_subsystem_listener_get_trid, const struct spdk_nvme_transport_id *,
	(struct spdk_nvmf_subsystem_listener *listener), NULL);
DEFINE_STUB(spdk_nvmf_subsystem_pause, int,
	(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid,
		spdk_nvmf_subsystem_state_change_done cb_fn, void *cb_arg), 0);
DEFINE_STUB(spdk_nvmf_subsystem_remove_host, int,
	(struct spdk_nvmf_subsystem *subsystem, const char *hostnqn), 0);
DEFINE_STUB(spdk_nvmf_subsystem_remove_listener, int,
	(struct spdk_nvmf_subsystem *subsystem, const struct spdk_nvme_transport_id *trid), 0);
DEFINE_STUB(spdk_nvmf_subsystem_remove_ns, int,
	(struct spdk_nvmf_subsystem *subsystem, uint32_t nsid), 0);
DEFINE_STUB(spdk_nvmf_subsystem_resume, int,
	(struct spdk_nvmf_subsystem *subsystem, spdk_nvmf_subsystem_state_change_done cb_fn,
		void *cb_arg), 0);
DEFINE_STUB(spdk_nvmf_subsystem_set_allow_any_host, int,
	(struct spdk_nvmf_subsystem *subsystem, bool allow_any_host), 0);
DEFINE_STUB(spdk_nvmf_subsystem_set_ana_reporting, int,
	(struct spdk_nvmf_subsystem *subsystem, bool ana_reporting), 0);
DEFINE_STUB(spdk_nvmf_subsystem_set_mn, int,
	(struct spdk_nvmf_subsystem *subsystem, const char *mn), 0);
DEFINE_STUB(spdk_nvmf_subsystem_set_sn, int,
	(struct spdk_nvmf_subsystem *subsystem, const char *sn), 0);
DEFINE_STUB(spdk_nvmf_subsystem_start, int,
	(struct spdk_nvmf_subsystem *subsystem, spdk_nvmf_subsystem_state_change_done cb_fn,
		void *cb_arg), 0);
DEFINE_STUB(spdk_nvmf_subsystem_stop, int,
	(struct spdk_nvmf_subsystem *subsystem, spdk_nvmf_subsystem_state_change_done cb_fn,
		void *cb_arg), 0);
DEFINE_STUB_V(spdk_nvmf_tgt_add_transport,
	(struct spdk_nvmf_tgt *tgt, struct spdk_nvmf_transport *transport,
		spdk_nvmf_tgt_add_transport_done_fn cb_fn, void *cb_arg));
DEFINE_STUB(spdk_nvmf_tgt_create, struct spdk_nvmf_tgt *,
	(struct spdk_nvmf_target_opts *opts), NULL);
DEFINE_STUB_V(spdk_nvmf_tgt_destroy,
	(struct spdk_nvmf_tgt *tgt, spdk_nvmf_tgt_destroy_done_fn *cb_fn, void *cb_arg));
DEFINE_STUB(spdk_nvmf_tgt_find_subsystem, struct spdk_nvmf_subsystem *,
	(struct spdk_nvmf_tgt *tgt, const char *subnqn), NULL);
DEFINE_STUB(spdk_nvmf_tgt_get_name, const char *, (struct spdk_nvmf_tgt *tgt), NULL);
DEFINE_STUB(spdk_nvmf_tgt_get_transport, struct spdk_nvmf_transport *,
	(struct spdk_nvmf_tgt *tgt, const char *transport_name), NULL);
DEFINE_STUB(spdk_nvmf_tgt_listen_ext, int,
	(struct spdk_nvmf_tgt *tgt, const struct spdk_nvme_transport_id *trid,
		struct spdk_nvmf_listen_opts *opts), 0);
DEFINE_STUB(spdk_nvmf_tgt_stop_listen, int,
	(struct spdk_nvmf_tgt *tgt, struct spdk_nvme_transport_id *trid), 0);
DEFINE_STUB(spdk_nvmf_transport_create, struct spdk_nvmf_transport *,
	(const char *transport_name, struct spdk_nvmf_transport_opts *opts), NULL);
DEFINE_STUB(spdk_nvmf_transport_destroy, int,
	(struct spdk_nvmf_transport *transport, spdk_nvmf_transport_destroy_done_cb cb_fn,
		void *cb_arg), 0);
DEFINE_STUB(spdk_nvmf_transport_get_first, struct spdk_nvmf_transport *,
	(struct spdk_nvmf_tgt *tgt), NULL);
DEFINE_STUB(spdk_nvmf_transport_get_next, struct spdk_nvmf_transport *,
	(struct spdk_nvmf_transport *transport), NULL);
DEFINE_STUB(spdk_nvmf_transport_opts_init, bool,
	(const char *transport_name, struct spdk_nvmf_transport_opts *opts, size_t opts_size), 0);
DEFINE_STUB(spdk_nvmf_transport_stop_listen_async, int,
	(struct spdk_nvmf_transport *transport, const struct spdk_nvme_transport_id *trid,
		struct spdk_nvmf_subsystem *subsystem, spdk_nvmf_tgt_subsystem_listen_done_fn cb_fn,
		void *cb_arg), 0);
DEFINE_STUB(spdk_uuid_fmt_lower, int,
	(char *uuid_str, size_t uuid_str_size, const struct spdk_uuid *uuid), 0);
DEFINE_STUB(spdk_uuid_parse, int, (struct spdk_uuid *uuid, const char *uuid_str), 0);
