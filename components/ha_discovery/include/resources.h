// Copyright 2026 vals. All rights reserved.
// SPDX-License-Identifier: MIT
#pragma once

#define MQTT_PARAM_COUNT 8
#define CHANNEL_COUNT 2

static const char s_sensor[]    = "sensor";
static const char s_number[]    = "number";
// class
static const char c_total[]         = "total";
static const char c_total_inc[]     = "total_increasing";
static const char c_measurement[]   = "measurement";
// device
static const char d_current[]   = "current";
static const char d_energy[]    = "energy";
static const char d_power[]     = "power";
static const char d_rpower[]    = "reactive_power";
static const char d_pf[]        = "power_factor";
static const char d_voltage[]   = "voltage";
static const char d_freq[]      = "frequency";
static const char d_rssi[]      = "signal_strength";
static const char d_timestamp[] = "timestamp";
static const char d_duration[]  = "duration";
// names & units
static const char n_voltage[]   = "Voltage";
static const char u_voltage[]   = "V";
static const char n_current[]   = "Current";
static const char u_current[]   = "A";
static const char n_power[]     = "Power";
static const char u_power[]     = "W";
static const char n_freq[]      = "Frequency";
static const char u_freq[]      = "Hz";
static const char n_pf[]        = "Power factor";
static const char u_pf[]        = "";
static const char n_energy[]    = "Energy";
static const char u_energy[]    = "kWh";
static const char n_rssi[]      = "RSSI";
static const char u_rssi[]      = "dBm";
static const char n_time[]      = "Last seen";
static const char n_mac[]       = "MAC Address";
static const char n_chip[]      = "Chip";
static const char n_e1[]        = "Set energy E1";
static const char u_e1[]        = "kWh";
static const char n_e2[]        = "Set energy E2";
static const char u_e2[]        = "kWh";
static const char n_e0[]        = "PZEM energy offset";
static const char u_e0[]        = "kWh";
static const char n_constant[]  = "Meter constant";
static const char u_constant[]  = "imp/kWh";
static const char n_mqtt_per[]  = "MQTT send period";
static const char n_stat_per[]  = "Stat send period";
static const char u_sec[]       = "s";
static const char n_imp[]       = "Impulses";
static const char u_imp[]       = "imp";
static const char n_ip[]        = "IP Address";
static const char n_total[]     = "Total";
// ids
static const char i_voltage[]   = "voltage";
static const char i_current[]   = "current";
static const char i_power[]     = "power";
static const char i_freq[]      = "frequency";
static const char i_pf[]        = "pf";
static const char i_energy[]    = "energy";
static const char i_rssi[]      = "rssi";
static const char i_time[]      = "timestamp";
static const char i_mac[]       = "mac";
static const char i_chip[]      = "chip_id";
static const char i_e1[]        = "set_energy1";
static const char i_e2[]        = "set_energy2";
static const char i_e0[]        = "energy_offset";
static const char i_mqtt_per[]  = "mqtt_period";
static const char i_stat_per[]  = "stat_period";
static const char i_energy0[]   = "energy0";
static const char i_constant[]  = "constant";
static const char i_imp0[]      = "imp0";
static const char i_ip[]        = "ip";
// category
static const char cat_diag[] = "diagnostic";
static const char cat_conf[] = "config";
// icons
static const char icon_counter[]    = "mdi:counter";
static const char icon_current[]    = "mdi:current-ac";
static const char icon_freq[]       = "mdi:sine-wave";
static const char icon_power[]      = "mdi:power-plug-outline";
static const char icon_energy[]     = "mdi:counter";
static const char icon_pf[]         = "mdi:percent-box-outline";
static const char icon_rssi[]       = "mdi:wifi";
static const char icon_period[]     = "mdi:timer-cog-outline";
static const char icon_last_seen[]  = "mdi:clock-outline";
static const char icon_voltage[]    = "mdi:flash-outline";
static const char icon_constant[]   = "mdi:calculator";

static const char *const GENERAL_ENTITIES[][MQTT_PARAM_COUNT] = {
    // type,    name,       sensor_id,  state_class,    dev_class,   unit,       cat,       icon
    // single sensors (ip, mac и chip_id - атрибуты сенсора RSSI, отдельных сущностей нет)
    {s_sensor,  n_voltage,  i_voltage,  c_measurement,  d_voltage,   u_voltage,  "",        icon_voltage},
    {s_sensor,  n_current,  i_current,  c_measurement,  d_current,   u_current,  "",        icon_current},
    {s_sensor,  n_power,    i_power,    c_measurement,  d_power,     u_power,    "",        icon_power},
    {s_sensor,  n_freq,     i_freq,     c_measurement,  d_freq,      u_freq,     "",        icon_freq},
    {s_sensor,  n_pf,       i_pf,       c_measurement,  d_pf,        u_pf,       "",        icon_pf},
    {s_sensor,  n_energy,   i_energy,   c_total_inc,    d_energy,    u_energy,   "",        icon_energy},
    {s_sensor,  n_time,     i_time,     "",             d_timestamp, "",         cat_diag,  icon_last_seen},
    {s_number,  n_e1,       i_e1,       "",             "",          u_e1,       cat_conf,  icon_counter},
    {s_number,  n_e2,       i_e2,       "",             "",          u_e2,       cat_conf,  icon_counter},
    {s_number,  n_e0,       i_e0,       "",             "",          u_e0,       cat_conf,  icon_counter},
    {s_number,  n_constant, i_constant, "",             "",          u_constant, cat_conf,  icon_constant},
    {s_number,  n_mqtt_per, i_mqtt_per, "",             d_duration,  u_sec,     cat_conf,  icon_period},
    {s_number,  n_stat_per, i_stat_per, "",             d_duration,  u_sec,      cat_conf,  icon_period},
    {s_sensor,  n_rssi,     i_rssi,     c_measurement,  d_rssi,      u_rssi,     cat_diag,  icon_rssi},
};

static const char *const CHANNEL_ENTITIES[][MQTT_PARAM_COUNT] = {
    // type,    name,       sensor_id,  state_class,    dev_class,   unit,       cat,       icon
    // [0] - основной сенсор канала (энергия, отображение), [1..] - его атрибуты (импульсы - основа учёта)
    {s_sensor, n_total,     i_energy0,  c_total,        d_energy,    u_energy,   "",        icon_counter},
    {s_sensor, n_imp,       i_imp0,     c_total_inc,    "",          u_imp,      "",        icon_counter}
};

static const char s_energy_e1[] = "Energy E1";
static const char s_energy_e2[] = "Energy E2";
static const char *const CHANNEL_NAMES[CHANNEL_COUNT] = {s_energy_e1, s_energy_e2};

static const char s_classic[]   = "Classic";
static const char s_mod[]       = "Lite";
static const char *const MODEL_NAMES[] = {s_classic, s_mod};


#define GENERAL_ENTITY_COUNT (sizeof(GENERAL_ENTITIES) / sizeof(GENERAL_ENTITIES[0]))
#define CHANNEL_ENTITY_COUNT (sizeof(CHANNEL_ENTITIES) / sizeof(CHANNEL_ENTITIES[0]))

/* Границы number-сущностей (без min/max HA использует 1..100). Ключ - sensor_id.
   Энергия (кВт*ч) везде с точностью 2 знака: шаг 0.01. */
typedef struct {
    const char *id;
    float min, max, step;
} number_limits_t;

static const number_limits_t NUMBER_LIMITS[] = {
    {"set_energy1",   0, 10000000, 0.01f},
    {"set_energy2",   0, 10000000, 0.01f},
    {"energy_offset", -10000000, 10000000, 0.01f},  /* коррекция показаний PZEM, может быть < 0 */
    {"constant",      1, 1000000, 1},
    {"mqtt_period",   60, 3600, 1},     /* pzem_period_s */
    {"stat_period",   60, 86400, 1},    /* stats_period_s */
};
 
 
