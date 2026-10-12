# eMeter32-3: итоговая сборка со всеми правками

- settings: stats_uri, stats_period_s, energy_offset (только из HA); SETTINGS_VERSION 10 (настройки один раз сбросятся);
  settings_save не пишет в NVS, если ничего не изменилось
- captive_portal: блок "Статистика HTTP" (URI + период); начальные E1/E2 с шагом 0.01;
  "глазик" для паролей Wi-Fi и MQTT (показывает только вводимый текст, сохранённые пароли устройство не отдаёт)
- pulse_counter: watch point на пределах PCNT (иначе accum_count не копит переполнение)
- stats_push: HTTP POST JSON; пустой stats_uri = выключено; энергия с 2 знаками
- mqtt_mgr: клиент MQTT; иерархия топиков <mqtt_topic>/<номер чипа>/...
- ha_discovery: учёт по импульсам; основной сенсор канала = энергия, импульсы - атрибут imp;
  RSSI с атрибутами ip, mac, chip_id; публикация канала на каждую 0.01 кВт*ч и по периоду pzem_period_s;
  Set energy - только команда (optimistic, без состояния); энергия публикуется и вводится с 2 знаками;
  миграция импульсов из прежнего топика <mqtt_topic>/<hostname>/...
- ha_resources: таблицы сущностей HA (из files/resources.h)
- main.c: счётчик, ha_discovery, mqtt, stats_push

Не входит в архив: partitions.csv, sdkconfig.defaults, platformio.ini (оставьте свои).
