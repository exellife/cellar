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
  ('ca-apt-deal',  'cat-apartments', 'deal',  'Тип сделки', 'enum', 1, 1, NULL, '["Продажа","Аренда долгосрочная","Аренда посуточно"]', 0),
  ('ca-apt-rooms', 'cat-apartments', 'rooms', 'Комнат', 'int', 1, 1, NULL, NULL, 1),
  ('ca-apt-area',  'cat-apartments', 'area',  'Площадь', 'number', 0, 1, 'м²', NULL, 2),
  ('ca-apt-floor', 'cat-apartments', 'floor', 'Этаж', 'int', 0, 1, NULL, NULL, 3),
  ('ca-apt-floors','cat-apartments', 'total_floors', 'Этажность дома', 'int', 0, 0, NULL, NULL, 4),
  ('ca-apt-furn',  'cat-apartments', 'furnished', 'Мебель', 'bool', 0, 1, NULL, NULL, 5);

-- houses
INSERT INTO category_attribute (id, category_id, key, label, type, required, filterable, unit, options, sort_order) VALUES
  ('ca-house-deal', 'cat-houses', 'deal', 'Тип сделки', 'enum', 1, 1, NULL, '["Продажа","Аренда долгосрочная","Аренда посуточно"]', 0),
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

-- ============================================================================
-- Taxonomy expansion (FEEDBACK ask 1, 2026-07-04): subcategories + filterable
-- attributes per frontend-apps/apps/classifieds/TAXONOMY.md. Работа/Услуги stay
-- bare (deferred). Real-estate `deal` is one shared 3-option facet across subcats.
-- Plants/homeware/beauty/other-animals are description-only (no structured attrs).
-- ============================================================================

INSERT INTO category (id, parent_id, slug, name, sort_order) VALUES
  ('cat-trucks',     'cat-transport',   'trucks',     'Грузовики и спецтехника', 2),
  ('cat-parts',      'cat-transport',   'parts',      'Запчасти и аксессуары',   3),
  ('cat-commercial', 'cat-realestate',  'commercial', 'Коммерческая недвижимость', 2),
  ('cat-land',       'cat-realestate',  'land',       'Земельные участки',       3),
  ('cat-garages',    'cat-realestate',  'garages',    'Гаражи и стоянки',        4),
  ('cat-tablets',    'cat-electronics', 'tablets',    'Планшеты',                2),
  ('cat-tv',         'cat-electronics', 'tv',         'ТВ и проекторы',          3),
  ('cat-audio',      'cat-electronics', 'audio',      'Аудио',                   4),
  ('cat-photo',      'cat-electronics', 'photo',      'Фото и видео',            5),
  ('cat-consoles',   'cat-electronics', 'consoles',   'Игровые приставки',       6),
  ('cat-furniture',  'cat-home',        'furniture',  'Мебель',                  0),
  ('cat-appliances', 'cat-home',        'appliances', 'Бытовая техника',         1),
  ('cat-build',      'cat-home',        'build',      'Ремонт и стройматериалы', 2),
  ('cat-plants',     'cat-home',        'plants',     'Растения',                3),
  ('cat-homeware',   'cat-home',        'homeware',   'Посуда, текстиль, декор', 4),
  ('cat-clothing',   'cat-personal',    'clothing',   'Одежда',                  0),
  ('cat-shoes',      'cat-personal',    'shoes',      'Обувь',                   1),
  ('cat-kids',       'cat-personal',    'kids',       'Детские товары',          2),
  ('cat-jewelry',    'cat-personal',    'jewelry',    'Часы, украшения, аксессуары', 3),
  ('cat-beauty',     'cat-personal',    'beauty',     'Красота и здоровье',      4),
  ('cat-dogs',       'cat-animals',     'dogs',       'Собаки',                  0),
  ('cat-cats',       'cat-animals',     'cats',       'Кошки',                   1),
  ('cat-birds',      'cat-animals',     'birds',      'Птицы',                   2),
  ('cat-pets-other', 'cat-animals',     'pets-other', 'Другие животные',         3),
  ('cat-pet-supplies','cat-animals',    'pet-supplies','Товары для животных',    4);

-- Additions to EXISTING subcategories (additive, non-required — safe for old data/tests)
INSERT INTO category_attribute (id, category_id, key, label, type, required, filterable, unit, options, depends_on, sort_order) VALUES
  ('ca-car-drive',   'cat-cars',     'drive',     'Привод',          'enum',   0, 1, NULL, '["Передний","Задний","Полный"]', NULL, 7),
  ('ca-car-engine',  'cat-cars',     'engine_l',  'Объём двигателя', 'number', 0, 1, 'л',  NULL, NULL, 8),
  ('ca-moto-type',   'cat-moto',     'type',      'Тип',             'enum',   0, 1, NULL, '["Мотоцикл","Скутер","Квадроцикл","Мопед"]', NULL, 3),
  ('ca-phone-model', 'cat-phones',   'model',     'Модель',          'text',   0, 0, NULL, NULL, 'brand', 2),
  ('ca-pc-brand',    'cat-computers','brand',     'Бренд',           'enum',   0, 1, NULL, '["Apple","Asus","Acer","HP","Lenovo","Dell","MSI","Прочие"]', NULL, 2),
  ('ca-pc-storage',  'cat-computers','storage_gb','Накопитель',      'int',    0, 1, 'ГБ', NULL, NULL, 3);

-- New-subcategory attributes (10-col form: no depends_on)
INSERT INTO category_attribute (id, category_id, key, label, type, required, filterable, unit, options, sort_order) VALUES
  ('ca-truck-type',  'cat-trucks',    'type',      'Тип',        'enum', 0, 1, NULL, '["Грузовик","Автобус","Спецтехника","Прицеп"]', 0),
  ('ca-truck-year',  'cat-trucks',    'year',      'Год выпуска','int',  0, 1, NULL, NULL, 1),
  ('ca-truck-mileage','cat-trucks',   'mileage',   'Пробег',     'int',  0, 1, 'км', NULL, 2),
  ('ca-parts-type',  'cat-parts',     'part_type', 'Категория',  'enum', 0, 1, NULL, '["Шины и диски","Двигатель","Кузовные","Электрика","Салон","Прочее"]', 0),

  ('ca-comm-deal',   'cat-commercial','deal',      'Тип сделки', 'enum', 1, 1, NULL, '["Продажа","Аренда долгосрочная","Аренда посуточно"]', 0),
  ('ca-comm-type',   'cat-commercial','comm_type', 'Тип объекта','enum', 0, 1, NULL, '["Офис","Магазин","Склад","Производство","Общепит"]', 1),
  ('ca-comm-area',   'cat-commercial','area',      'Площадь',    'number',0, 1, 'м²', NULL, 2),
  ('ca-land-deal',   'cat-land',      'deal',      'Тип сделки', 'enum', 1, 1, NULL, '["Продажа","Аренда долгосрочная","Аренда посуточно"]', 0),
  ('ca-land-area',   'cat-land',      'land',      'Площадь участка','number',1,1,'сот.', NULL, 1),
  ('ca-land-purpose','cat-land',      'purpose',   'Назначение', 'enum', 0, 1, NULL, '["ИЖС","Сельхоз","Коммерческое"]', 2),
  ('ca-garage-deal', 'cat-garages',   'deal',      'Тип сделки', 'enum', 1, 1, NULL, '["Продажа","Аренда долгосрочная","Аренда посуточно"]', 0),
  ('ca-garage-type', 'cat-garages',   'garage_type','Тип',       'enum', 0, 1, NULL, '["Гараж","Парковочное место","Бокс"]', 1),

  ('ca-tab-brand',   'cat-tablets',   'brand',     'Бренд',      'enum', 0, 1, NULL, '["Apple","Samsung","Xiaomi","Huawei","Lenovo","Прочие"]', 0),
  ('ca-tab-storage', 'cat-tablets',   'storage',   'Память',     'enum', 0, 1, 'ГБ', '["32","64","128","256","512","1024"]', 1),
  ('ca-tv-brand',    'cat-tv',        'brand',     'Бренд',      'enum', 0, 1, NULL, '["Samsung","LG","Sony","Xiaomi","TCL","Прочие"]', 0),
  ('ca-tv-screen',   'cat-tv',        'screen_in', 'Диагональ',  'int',  0, 1, '"',  NULL, 1),
  ('ca-tv-panel',    'cat-tv',        'panel',     'Матрица',    'enum', 0, 1, NULL, '["LED","OLED","QLED"]', 2),
  ('ca-audio-type',  'cat-audio',     'audio_type','Тип',        'enum', 0, 1, NULL, '["Наушники","Колонки","Саундбар","Микрофоны"]', 0),
  ('ca-photo-type',  'cat-photo',     'cam_type',  'Тип',        'enum', 0, 1, NULL, '["Зеркальные","Беззеркальные","Компактные","Экшн"]', 0),
  ('ca-console-brand','cat-consoles', 'brand',     'Бренд',      'enum', 0, 1, NULL, '["PlayStation","Xbox","Nintendo"]', 0),

  ('ca-furn-type',   'cat-furniture', 'furn_type', 'Тип',        'enum', 0, 1, NULL, '["Диваны","Кровати","Шкафы","Столы и стулья","Кухни","Детская"]', 0),
  ('ca-appl-type',   'cat-appliances','appliance_type','Тип',    'enum', 0, 1, NULL, '["Холодильники","Стиральные","Плиты","Посудомоечные","Микроволновки","Кондиционеры","Пылесосы"]', 0),
  ('ca-build-type',  'cat-build',     'build_type','Категория',  'enum', 0, 1, NULL, '["Инструменты","Стройматериалы","Сантехника","Электрика","Двери и окна"]', 0),

  ('ca-cloth-gender','cat-clothing',  'gender',    'Пол',        'enum', 1, 1, NULL, '["Мужская","Женская","Детская","Унисекс"]', 0),
  ('ca-cloth-size',  'cat-clothing',  'size',      'Размер',     'enum', 0, 1, NULL, '["XS","S","M","L","XL","XXL","XXXL"]', 1),
  ('ca-shoe-gender', 'cat-shoes',     'gender',    'Пол',        'enum', 1, 1, NULL, '["Мужская","Женская","Детская","Унисекс"]', 0),
  ('ca-shoe-size',   'cat-shoes',     'shoe_size', 'Размер',     'int',  0, 1, NULL, NULL, 1),
  ('ca-kids-type',   'cat-kids',      'kids_type', 'Категория',  'enum', 0, 1, NULL, '["Коляски","Автокресла","Игрушки","Одежда","Мебель"]', 0),
  ('ca-kids-age',    'cat-kids',      'age_group', 'Возраст',    'enum', 0, 1, NULL, '["0–1","1–3","3–6","6+"]', 1),
  ('ca-jew-type',    'cat-jewelry',   'acc_type',  'Тип',        'enum', 0, 1, NULL, '["Часы","Украшения","Сумки","Очки","Прочее"]', 0),

  ('ca-dogs-breed',  'cat-dogs',      'breed',     'Порода',     'text', 0, 1, NULL, NULL, 0),
  ('ca-dogs-purpose','cat-dogs',      'purpose',   'Цель',       'enum', 0, 1, NULL, '["Продажа","Вязка"]', 1),
  ('ca-cats-breed',  'cat-cats',      'breed',     'Порода',     'text', 0, 1, NULL, NULL, 0),
  ('ca-cats-purpose','cat-cats',      'purpose',   'Цель',       'enum', 0, 1, NULL, '["Продажа","Вязка"]', 1),
  ('ca-birds-breed', 'cat-birds',     'breed',     'Вид',        'text', 0, 1, NULL, NULL, 0),
  ('ca-pet-supply',  'cat-pet-supplies','pet_supply','Категория','enum', 0, 1, NULL, '["Корма","Аксессуары","Клетки и аквариумы","Уход"]', 0);
