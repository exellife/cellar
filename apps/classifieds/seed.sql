-- ============================================================================
-- classifieds — seed data (A0.6): KG geo tree + a starter category taxonomy
-- with per-category attributes. Loaded AFTER schema.sql. Ids are stable slugs
-- (readable; the uuid4 DEFAULT only mints ids for API-created rows like listings).
--
-- Names are Russian (the lingua franca of KG classifieds); `labels` carries a
-- {ru,ky} pair where a Kyrgyz label is worth showing. Extend freely — the whole
-- point of the data model is that this is DATA, not code (no migration to add a
-- category or an attribute).
-- ============================================================================

-- ── Geo: oblast → city → district (Kyrgyzstan) ─────────────────────────────
-- Top level mixes the 7 oblasts with the two republic-level cities (Bishkek,
-- Osh) so users can pick the big cities directly.
INSERT INTO geo_oblast (id, name, labels, sort_order) VALUES
  ('ob-bishkek',   'Бишкек',            '{"ky":"Бишкек"}',            0),
  ('ob-osh-city',  'Ош (город)',        '{"ky":"Ош шаары"}',          1),
  ('ob-chuy',      'Чуйская область',   '{"ky":"Чүй облусу"}',        2),
  ('ob-osh',       'Ошская область',    '{"ky":"Ош облусу"}',         3),
  ('ob-jalalabad', 'Джалал-Абадская область', '{"ky":"Жалал-Абад облусу"}', 4),
  ('ob-issykkul',  'Иссык-Кульская область',  '{"ky":"Ысык-Көл облусу"}',   5),
  ('ob-naryn',     'Нарынская область', '{"ky":"Нарын облусу"}',      6),
  ('ob-talas',     'Таласская область', '{"ky":"Талас облусу"}',      7),
  ('ob-batken',    'Баткенская область','{"ky":"Баткен облусу"}',     8);

INSERT INTO geo_city (id, oblast_id, name, sort_order) VALUES
  ('ci-bishkek',   'ob-bishkek',   'Бишкек',      0),
  ('ci-osh',       'ob-osh-city',  'Ош',          0),
  ('ci-tokmok',    'ob-chuy',      'Токмок',      0),
  ('ci-kara-balta','ob-chuy',      'Кара-Балта',  1),
  ('ci-kant',      'ob-chuy',      'Кант',        2),
  ('ci-uzgen',     'ob-osh',       'Узген',       0),
  ('ci-jalalabad', 'ob-jalalabad', 'Джалал-Абад', 0),
  ('ci-karakol',   'ob-issykkul',  'Каракол',     0),
  ('ci-balykchy',  'ob-issykkul',  'Балыкчы',     1),
  ('ci-cholponata','ob-issykkul',  'Чолпон-Ата',  2),
  ('ci-naryn',     'ob-naryn',     'Нарын',       0),
  ('ci-talas',     'ob-talas',     'Талас',       0),
  ('ci-batken',    'ob-batken',    'Баткен',      0);

-- Bishkek districts (the district level; other cities can be filled in later).
INSERT INTO geo_district (id, city_id, name, sort_order) VALUES
  ('di-leninsky',    'ci-bishkek', 'Ленинский район',    0),
  ('di-oktyabrsky',  'ci-bishkek', 'Октябрьский район',  1),
  ('di-pervomaysky', 'ci-bishkek', 'Первомайский район', 2),
  ('di-sverdlovsky', 'ci-bishkek', 'Свердловский район', 3);

-- ── Category taxonomy (top level) ──────────────────────────────────────────
INSERT INTO category (id, parent_id, slug, name, labels, sort_order) VALUES
  ('cat-transport',   NULL, 'transport',   'Транспорт',       '{"ky":"Транспорт"}',   0),
  ('cat-realestate',  NULL, 'realestate',  'Недвижимость',    '{"ky":"Кыймылсыз мүлк"}', 1),
  ('cat-electronics', NULL, 'electronics', 'Электроника',     '{"ky":"Электроника"}', 2),
  ('cat-home',        NULL, 'home',        'Дом и сад',       NULL, 3),
  ('cat-personal',    NULL, 'personal',    'Личные вещи',     NULL, 4),
  ('cat-animals',     NULL, 'animals',     'Животные',        NULL, 5),
  ('cat-jobs',        NULL, 'jobs',        'Работа',          NULL, 6),
  ('cat-services',    NULL, 'services',    'Услуги',          NULL, 7);

-- Subcategories (a few, to exercise the tree + per-category attributes).
INSERT INTO category (id, parent_id, slug, name, sort_order) VALUES
  ('cat-cars',       'cat-transport',   'cars',       'Автомобили',  0),
  ('cat-moto',       'cat-transport',   'moto',       'Мотоциклы',   1),
  ('cat-apartments', 'cat-realestate',  'apartments', 'Квартиры',    0),
  ('cat-houses',     'cat-realestate',  'houses',     'Дома',        1),
  ('cat-phones',     'cat-electronics', 'phones',     'Телефоны',    0),
  ('cat-computers',  'cat-electronics', 'computers',  'Компьютеры',  1);

-- ── Per-category attributes (the form + facets + validation, all from data) ─
-- cars
INSERT INTO category_attribute (id, category_id, key, label, type, required, filterable, unit, options, depends_on, sort_order) VALUES
  ('ca-car-make',  'cat-cars', 'make',  'Марка',   'enum', 1, 1, NULL,
     '["Toyota","Honda","Nissan","Mercedes-Benz","BMW","Audi","Lexus","Hyundai","Kia","Lada","Mitsubishi","Volkswagen"]', NULL, 0),
  ('ca-car-model', 'cat-cars', 'model', 'Модель',  'text', 0, 1, NULL, NULL, 'make', 1),
  ('ca-car-year',  'cat-cars', 'year',  'Год выпуска', 'int', 1, 1, NULL, NULL, NULL, 2),
  ('ca-car-mileage','cat-cars','mileage','Пробег', 'int', 0, 1, 'км', NULL, NULL, 3),
  ('ca-car-trans', 'cat-cars', 'transmission', 'Коробка передач', 'enum', 0, 1, NULL,
     '["Механика","Автомат","Вариатор","Робот"]', NULL, 4),
  ('ca-car-fuel',  'cat-cars', 'fuel',  'Топливо', 'enum', 0, 1, NULL,
     '["Бензин","Дизель","Газ","Гибрид","Электро"]', NULL, 5),
  ('ca-car-body',  'cat-cars', 'body',  'Кузов',   'enum', 0, 1, NULL,
     '["Седан","Хэтчбек","Универсал","Внедорожник","Минивэн","Купе","Пикап"]', NULL, 6);

-- motorcycles
INSERT INTO category_attribute (id, category_id, key, label, type, required, filterable, unit, options, sort_order) VALUES
  ('ca-moto-make', 'cat-moto', 'make', 'Марка', 'enum', 1, 1, NULL,
     '["Honda","Yamaha","Suzuki","Kawasaki","BMW","Racer","Other"]', 0),
  ('ca-moto-year', 'cat-moto', 'year', 'Год выпуска', 'int', 1, 1, NULL, NULL, 1),
  ('ca-moto-engine','cat-moto','engine_cc','Объём двигателя','int',0,1,'см³', NULL, 2);

-- apartments
INSERT INTO category_attribute (id, category_id, key, label, type, required, filterable, unit, options, sort_order) VALUES
  ('ca-apt-deal',  'cat-apartments', 'deal',  'Тип сделки', 'enum', 1, 1, NULL, '["Продажа","Аренда"]', 0),
  ('ca-apt-rooms', 'cat-apartments', 'rooms', 'Комнат', 'int', 1, 1, NULL, NULL, 1),
  ('ca-apt-area',  'cat-apartments', 'area',  'Площадь', 'number', 0, 1, 'м²', NULL, 2),
  ('ca-apt-floor', 'cat-apartments', 'floor', 'Этаж', 'int', 0, 1, NULL, NULL, 3),
  ('ca-apt-floors','cat-apartments', 'total_floors', 'Этажность дома', 'int', 0, 0, NULL, NULL, 4),
  ('ca-apt-furn',  'cat-apartments', 'furnished', 'Мебель', 'bool', 0, 1, NULL, NULL, 5);

-- houses
INSERT INTO category_attribute (id, category_id, key, label, type, required, filterable, unit, options, sort_order) VALUES
  ('ca-house-deal', 'cat-houses', 'deal', 'Тип сделки', 'enum', 1, 1, NULL, '["Продажа","Аренда"]', 0),
  ('ca-house-rooms','cat-houses', 'rooms','Комнат', 'int', 0, 1, NULL, NULL, 1),
  ('ca-house-area', 'cat-houses', 'area', 'Площадь дома', 'number', 0, 1, 'м²', NULL, 2),
  ('ca-house-land', 'cat-houses', 'land', 'Площадь участка', 'number', 0, 1, 'сот.', NULL, 3);

-- phones
INSERT INTO category_attribute (id, category_id, key, label, type, required, filterable, unit, options, sort_order) VALUES
  ('ca-phone-brand', 'cat-phones', 'brand', 'Бренд', 'enum', 1, 1, NULL,
     '["Apple","Samsung","Xiaomi","Huawei","Honor","Realme","OPPO","Vivo","Nokia"]', 0),
  ('ca-phone-storage','cat-phones','storage','Память', 'enum', 0, 1, 'ГБ', '["32","64","128","256","512","1024"]', 1);

-- computers
INSERT INTO category_attribute (id, category_id, key, label, type, required, filterable, unit, options, sort_order) VALUES
  ('ca-pc-kind', 'cat-computers', 'kind', 'Тип', 'enum', 1, 1, NULL,
     '["Ноутбук","Моноблок","Системный блок","Комплектующие"]', 0),
  ('ca-pc-ram',  'cat-computers', 'ram', 'Оперативная память', 'enum', 0, 1, 'ГБ', '["4","8","16","32","64"]', 1);
