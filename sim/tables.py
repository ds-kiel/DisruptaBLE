from pydal import Field


def init_tables(db):
    db.define_table(
        'run',
        Field('name', notnull=True),
        Field('group'),
        Field('start_ts', type='datetime', notnull=True),
        Field('end_ts', type='datetime'),
        Field('status', notnull=True),
        Field('seed', type='bigint', notnull=True),
        Field('simulation_time', type='bigint', notnull=True),
        Field('progress', type='bigint', notnull=True),
        Field('num_proxy_devices', type='integer', notnull=True),
        redefine=False
    )

    db.define_table(
        'event',
        Field('run', 'reference run', notnull=True),
        Field('type', notnull=True),
        Field('device',  type='integer', notnull=True),
        Field('us',  type='bigint', notnull=True),
        Field('data_json',  type='text', notnull=True),
        redefine=False
    )

def init_eval_tables(db):
    # Actual parsed data
    db.define_table(
        'device',
        Field('run', 'reference run', notnull=True),
        Field('number',  type='integer', notnull=True),
        Field('eid',  notnull=True),
        Field('mac_addr',  notnull=True),
        redefine=False
    )

    db.define_table(
        'conn_info',
        Field('run', 'reference run', notnull=True),
        Field('client', 'reference device', notnull=True),
        Field('peripheral', 'reference device', notnull=True),
        Field('client_rx_bytes', type='integer'),
        Field('client_tx_bytes', type='integer'),
        Field('client_conn_init_us', type='bigint'),
        Field('client_channel_up_us', type='bigint'),
        Field('client_channel_down_us', type='bigint'),
        Field('client_connection_success_us', type='bigint'),
        Field('client_connection_failure_us', type='bigint'),
        Field('client_disconnect_us', type='bigint'),
        Field('client_disconnect_reason'),
        Field('client_idle_disconnect_us', type='bigint'),
        Field('peripheral_rx_bytes', type='integer'),
        Field('peripheral_tx_bytes', type='integer'),
        Field('peripheral_channel_up_us', type='bigint'),
        Field('peripheral_channel_down_us', type='bigint'),
        Field('peripheral_connection_success_us', type='bigint'),
        Field('peripheral_connection_failure_us', type='bigint'),
        Field('peripheral_disconnect_us', type='bigint'),
        Field('peripheral_disconnect_reason', type='bigint'),
        Field('peripheral_idle_disconnect_us', type='bigint'),
        redefine=False
    )

    db.define_table(
        'bundle',
        Field('run', 'reference run', notnull=True),
        Field('source', 'reference device', notnull=True),
        Field('destination_eid'),
        Field('source_eid'),
        Field('destination', 'reference device', notnull=False),
        Field('creation_timestamp_ms', type='integer'),
        Field('payload_length', type='integer'),
        Field('is_sv', type='boolean'),
        Field('lifetime_ms', type='integer'),
        Field('hop_count', type='integer'),
        redefine=False
    )

    db.define_table(
        'stored_bundle',
        Field('run', 'reference run', notnull=True),
        Field('device', 'reference device', notnull=True),
        Field('bundle', 'reference bundle', notnull=True),
        Field('created_us', type='bigint', notnull=True),
        Field('local_id', type='integer', notnull=True),
        Field('deleted_us', type='bigint', notnull=False),
        Field('remaining_hops', type='integer', notnull=False),
        redefine=False
    )

    db.define_table(
        'bundle_transmission',
        Field('run', 'reference run', notnull=True),
        Field('conn_info', 'reference conn_info', notnull=True),
        Field('source_stored_bundle', 'reference stored_bundle', notnull=True),
        Field('received_stored_bundle', 'reference stored_bundle', notnull=False),
        Field('start_us',  type='bigint'),
        Field('end_us',  type='bigint'),
        redefine=False
    )

    # TODO: Extra handling of summary vectors?

def reset_eval_tables(db):
    eval_tables = [
        'bundle_transmission',
        'stored_bundle',
        'bundle',
        'conn_info',
        'device'    #last since every other reference this one
    ]

    for t in eval_tables:
        db[t].drop()

    db.commit()