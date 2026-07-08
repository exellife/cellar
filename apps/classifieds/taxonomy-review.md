# Taxonomy de-Russian — review draft

Codes = English slugs derived from the frontend's English spec labels (Latin option values —
brands/numbers/sizes — kept as-is). KY from ky-taxonomy.draft.json (option KY is their Phase 2).

## Categories
| id | RU (now) | EN | KY |
|---|---|---|---|
| `cat-transport` | Транспорт | **Transport** |  |
| `cat-realestate` | Недвижимость | **Real Estate** |  |
| `cat-electronics` | Электроника | **Electronics** |  |
| `cat-home` | Дом и сад | **Home & Garden** | Үй жана бак |
| `cat-personal` | Личные вещи | **Personal Items** | Жеке буюмдар |
| `cat-animals` | Животные | **Animals** | Жаныбарлар |
| `cat-jobs` | Работа | **Jobs** | Жумуш |
| `cat-services` | Услуги | **Services** | Кызматтар |
| `cat-cars` | Автомобили | **Cars** | Автоунаалар |
| `cat-apartments` | Квартиры | **Apartments** | Батирлер |
| `cat-phones` | Телефоны | **Phones** | Телефондор |
| `cat-furniture` | Мебель | **Furniture** | Эмерек |
| `cat-clothing` | Одежда | **Clothing** | Кийим |
| `cat-dogs` | Собаки | **Dogs** | Иттер |
| `cat-moto` | Мотоциклы | **Motorcycles** | Мотоциклдер |
| `cat-houses` | Дома | **Houses** | Үйлөр |
| `cat-computers` | Компьютеры | **Computers** | Компьютерлер |
| `cat-appliances` | Бытовая техника | **Appliances** | Тиричилик техникасы |
| `cat-shoes` | Обувь | **Shoes** | Бут кийим |
| `cat-cats` | Кошки | **Cats** | Мышыктар |
| `cat-trucks` | Грузовики и спецтехника | **Trucks & machinery** | Жүк унаалары жана атайын техника |
| `cat-commercial` | Коммерческая недвижимость | **Commercial** | Коммерциялык кыймылсыз мүлк |
| `cat-tablets` | Планшеты | **Tablets** | Планшеттер |
| `cat-build` | Ремонт и стройматериалы | **Repair & building materials** | Оңдоо жана курулуш материалдары |
| `cat-kids` | Детские товары | **Kids** | Балдар товарлары |
| `cat-birds` | Птицы | **Birds** | Канаттуулар |
| `cat-parts` | Запчасти и аксессуары | **Parts & accessories** | Тетиктер жана аксессуарлар |
| `cat-land` | Земельные участки | **Land** | Жер тилкелери |
| `cat-tv` | ТВ и проекторы | **TV & projectors** | ТВ жана проекторлор |
| `cat-plants` | Растения | **Plants** | Өсүмдүктөр |
| `cat-jewelry` | Часы, украшения, аксессуары | **Watches & accessories** | Саат, зер буюмдары, аксессуарлар |
| `cat-pets-other` | Другие животные | **Other animals** | Башка жаныбарлар |
| `cat-garages` | Гаражи и стоянки | **Garages** | Гараждар жана токтотуучу жайлар |
| `cat-audio` | Аудио | **Audio** | Аудио |
| `cat-homeware` | Посуда, текстиль, декор | **Homeware** | Идиш-аяк, текстиль, декор |
| `cat-beauty` | Красота и здоровье | **Beauty & health** | Сулуулук жана ден соолук |
| `cat-pet-supplies` | Товары для животных | **Pet supplies** | Жаныбарлар үчүн товарлар |
| `cat-photo` | Фото и видео | **Photo & video** | Фото жана видео |
| `cat-consoles` | Игровые приставки | **Consoles** | Оюн приставкалары |
| `cat-smartwatch` | Умные часы | **Smartwatches** | Акылдуу сааттар |

## Attributes
| cat | key | RU label | EN label | KY |
|---|---|---|---|---|
| cat-apartments | `deal` | Тип сделки | **Deal** | Бүтүм түрү |
| cat-apartments | `rooms` | Комнат | **Rooms** | Бөлмө |
| cat-apartments | `area` | Площадь | **Area** | Аянты |
| cat-apartments | `floor` | Этаж | **Floor** | Кабат |
| cat-apartments | `total_floors` | Этажность дома | **Total floors** | Үйдүн кабаттыгы |
| cat-apartments | `furnished` | Мебель | **Furnished** | Эмерек |
| cat-appliances | `appliance_type` | Тип | **Type** | Түрү |
| cat-audio | `audio_type` | Тип | **Type** | Түрү |
| cat-birds | `breed` | Вид | **Species** | Түрү |
| cat-build | `build_type` | Категория | **Category** | Категория |
| cat-cars | `make` | Марка | **Make** | Марка |
| cat-cars | `model` | Модель | **Model** | Модель |
| cat-cars | `year` | Год выпуска | **Year** | Чыгарылган жылы |
| cat-cars | `mileage` | Пробег | **Mileage** | Жүрүшү |
| cat-cars | `transmission` | Коробка передач | **Transmission** | Берүү кутусу |
| cat-cars | `fuel` | Топливо | **Fuel** | Күйүүчү май |
| cat-cars | `body` | Кузов | **Body** | Кузов |
| cat-cars | `drive` | Привод | **Drivetrain** | Жүргүзгүч |
| cat-cars | `engine_l` | Объём двигателя | **Engine** | Кыймылдаткычтын көлөмү |
| cat-cats | `breed` | Порода | **Breed** | Тукуму |
| cat-cats | `purpose` | Цель | **Purpose** | Максаты |
| cat-clothing | `gender` | Пол | **Gender** | Жынысы |
| cat-clothing | `size` | Размер | **Size** | Өлчөмү |
| cat-commercial | `deal` | Тип сделки | **Deal** | Бүтүм түрү |
| cat-commercial | `comm_type` | Тип объекта | **Type** | Объекттин түрү |
| cat-commercial | `area` | Площадь | **Area** | Аянты |
| cat-computers | `kind` | Тип | **Type** | Түрү |
| cat-computers | `ram` | Оперативная память | **RAM** | RAM |
| cat-computers | `brand` | Бренд | **Brand** | Бренд |
| cat-computers | `storage_gb` | Накопитель | **Storage** | Диск |
| cat-computers | `cpu_brand` | Процессор (бренд) | **CPU** | CPU |
| cat-computers | `cpu_model` | Модель процессора | **CPU model** | Процессордун модели |
| cat-consoles | `brand` | Бренд | **Brand** | Бренд |
| cat-dogs | `breed` | Порода | **Breed** | Тукуму |
| cat-dogs | `purpose` | Цель | **Purpose** | Максаты |
| cat-furniture | `furn_type` | Тип | **Type** | Түрү |
| cat-garages | `deal` | Тип сделки | **Deal** | Бүтүм түрү |
| cat-garages | `garage_type` | Тип | **Type** | Түрү |
| cat-houses | `deal` | Тип сделки | **Deal** | Бүтүм түрү |
| cat-houses | `rooms` | Комнат | **Rooms** | Бөлмө |
| cat-houses | `area` | Площадь дома | **Area** | Үйдүн аянты |
| cat-houses | `land` | Площадь участка | **Land** | Тилкенин аянты |
| cat-jewelry | `acc_type` | Тип | **Type** | Түрү |
| cat-kids | `kids_type` | Категория | **Category** | Категория |
| cat-kids | `age_group` | Возраст | **Age** | Жашы |
| cat-land | `deal` | Тип сделки | **Deal** | Бүтүм түрү |
| cat-land | `land` | Площадь участка | **Area** | Тилкенин аянты |
| cat-land | `purpose` | Назначение | **Purpose** | Багыты |
| cat-moto | `make` | Марка | **Make** | Марка |
| cat-moto | `year` | Год выпуска | **Year** | Чыгарылган жылы |
| cat-moto | `engine_cc` | Объём двигателя | **Engine** | Кыймылдаткычтын көлөмү |
| cat-moto | `type` | Тип | **Type** | Түрү |
| cat-parts | `part_type` | Категория | **Category** | Категория |
| cat-pet-supplies | `pet_supply` | Категория | **Category** | Категория |
| cat-phones | `brand` | Бренд | **Brand** | Бренд |
| cat-phones | `storage` | Память | **Storage** | Эстутум |
| cat-phones | `model` | Модель | **Model** | Модель |
| cat-photo | `cam_type` | Тип | **Type** | Түрү |
| cat-shoes | `gender` | Пол | **Gender** | Жынысы |
| cat-shoes | `shoe_size` | Размер | **Size** | Өлчөмү |
| cat-smartwatch | `brand` | Бренд | **Brand** | Бренд |
| cat-tablets | `brand` | Бренд | **Brand** | Бренд |
| cat-tablets | `storage` | Память | **Storage** | Эстутум |
| cat-trucks | `type` | Тип | **Type** | Түрү |
| cat-trucks | `year` | Год выпуска | **Year** | Чыгарылган жылы |
| cat-trucks | `mileage` | Пробег | **Mileage** | Жүрүшү |
| cat-tv | `brand` | Бренд | **Brand** | Бренд |
| cat-tv | `screen_in` | Диагональ | **Screen** | Диагональ |
| cat-tv | `panel` | Матрица | **Panel** | Матрица |

## Enum option codes (RU value → slug → EN label)
| cat.key | RU value | → slug (code) | EN label |
|---|---|---|---|
| cat-apartments.deal | Продажа | **sale** | Sale |
| cat-apartments.deal | Аренда долгосрочная | **long-term-rent** | Long-term rent |
| cat-apartments.deal | Аренда посуточно | **daily-rent** | Daily rent |
| cat-appliances.appliance_type | Холодильники | **refrigerators** | Refrigerators |
| cat-appliances.appliance_type | Стиральные | **washers** | Washers |
| cat-appliances.appliance_type | Плиты | **stoves** | Stoves |
| cat-appliances.appliance_type | Посудомоечные | **dishwashers** | Dishwashers |
| cat-appliances.appliance_type | Микроволновки | **microwaves** | Microwaves |
| cat-appliances.appliance_type | Кондиционеры | **air-conditioners** | Air conditioners |
| cat-appliances.appliance_type | Пылесосы | **vacuums** | Vacuums |
| cat-audio.audio_type | Наушники | **headphones** | Headphones |
| cat-audio.audio_type | Колонки | **speakers** | Speakers |
| cat-audio.audio_type | Саундбар | **soundbar** | Soundbar |
| cat-audio.audio_type | Микрофоны | **microphones** | Microphones |
| cat-build.build_type | Инструменты | **tools** | Tools |
| cat-build.build_type | Стройматериалы | **building-materials** | Building materials |
| cat-build.build_type | Сантехника | **plumbing** | Plumbing |
| cat-build.build_type | Электрика | **electrical** | Electrical |
| cat-build.build_type | Двери и окна | **doors-and-windows** | Doors & windows |
| cat-cars.make | `Toyota` | _(Latin — keep)_ | `Toyota` |
| cat-cars.make | `Honda` | _(Latin — keep)_ | `Honda` |
| cat-cars.make | `Nissan` | _(Latin — keep)_ | `Nissan` |
| cat-cars.make | `Mercedes-Benz` | _(Latin — keep)_ | `Mercedes-Benz` |
| cat-cars.make | `BMW` | _(Latin — keep)_ | `BMW` |
| cat-cars.make | `Audi` | _(Latin — keep)_ | `Audi` |
| cat-cars.make | `Lexus` | _(Latin — keep)_ | `Lexus` |
| cat-cars.make | `Hyundai` | _(Latin — keep)_ | `Hyundai` |
| cat-cars.make | `Kia` | _(Latin — keep)_ | `Kia` |
| cat-cars.make | `Lada` | _(Latin — keep)_ | `Lada` |
| cat-cars.make | `Mitsubishi` | _(Latin — keep)_ | `Mitsubishi` |
| cat-cars.make | `Volkswagen` | _(Latin — keep)_ | `Volkswagen` |
| cat-cars.transmission | Механика | **manual** | Manual |
| cat-cars.transmission | Автомат | **automatic** | Automatic |
| cat-cars.transmission | Вариатор | **cvt** | CVT |
| cat-cars.transmission | Робот | **amt** | AMT |
| cat-cars.fuel | Бензин | **petrol** | Petrol |
| cat-cars.fuel | Дизель | **diesel** | Diesel |
| cat-cars.fuel | Газ | **lpg** ⚠ | Gas |
| cat-cars.fuel | Гибрид | **hybrid** | Hybrid |
| cat-cars.fuel | Электро | **electric** | Electric |
| cat-cars.body | Седан | **sedan** | Sedan |
| cat-cars.body | Хэтчбек | **hatchback** | Hatchback |
| cat-cars.body | Универсал | **wagon** | Wagon |
| cat-cars.body | Внедорожник | **suv** | SUV |
| cat-cars.body | Минивэн | **minivan** | Minivan |
| cat-cars.body | Купе | **coupe** | Coupe |
| cat-cars.body | Пикап | **pickup** | Pickup |
| cat-cars.drive | Передний | **fwd** | FWD |
| cat-cars.drive | Задний | **rwd** | RWD |
| cat-cars.drive | Полный | **4x4** | 4x4 |
| cat-cats.purpose | Продажа | **sale** | Sale |
| cat-cats.purpose | Вязка | **mating** | Mating |
| cat-clothing.gender | Мужская | **men** | Men |
| cat-clothing.gender | Женская | **women** | Women |
| cat-clothing.gender | Детская | **kids** | Kids |
| cat-clothing.gender | Унисекс | **unisex** | Unisex |
| cat-clothing.size | `XS` | _(Latin — keep)_ | `XS` |
| cat-clothing.size | `S` | _(Latin — keep)_ | `S` |
| cat-clothing.size | `M` | _(Latin — keep)_ | `M` |
| cat-clothing.size | `L` | _(Latin — keep)_ | `L` |
| cat-clothing.size | `XL` | _(Latin — keep)_ | `XL` |
| cat-clothing.size | `XXL` | _(Latin — keep)_ | `XXL` |
| cat-clothing.size | `XXXL` | _(Latin — keep)_ | `XXXL` |
| cat-commercial.deal | Продажа | **sale** | Sale |
| cat-commercial.deal | Аренда долгосрочная | **long-term-rent** | Long-term rent |
| cat-commercial.deal | Аренда посуточно | **daily-rent** | Daily rent |
| cat-commercial.comm_type | Офис | **office** | Office |
| cat-commercial.comm_type | Магазин | **shop** | Shop |
| cat-commercial.comm_type | Склад | **warehouse** | Warehouse |
| cat-commercial.comm_type | Производство | **production** | Production |
| cat-commercial.comm_type | Общепит | **catering** | Catering |
| cat-computers.kind | Ноутбук | **laptop** | Laptop |
| cat-computers.kind | Моноблок | **all-in-one** | All-in-One |
| cat-computers.kind | Системный блок | **desktop** | Desktop |
| cat-computers.kind | Комплектующие | **components** | Components |
| cat-computers.ram | `4` | _(Latin — keep)_ | `4` |
| cat-computers.ram | `8` | _(Latin — keep)_ | `8` |
| cat-computers.ram | `16` | _(Latin — keep)_ | `16` |
| cat-computers.ram | `32` | _(Latin — keep)_ | `32` |
| cat-computers.ram | `64` | _(Latin — keep)_ | `64` |
| cat-computers.brand | `Apple` | _(Latin — keep)_ | `Apple` |
| cat-computers.brand | `Asus` | _(Latin — keep)_ | `Asus` |
| cat-computers.brand | `Acer` | _(Latin — keep)_ | `Acer` |
| cat-computers.brand | `HP` | _(Latin — keep)_ | `HP` |
| cat-computers.brand | `Lenovo` | _(Latin — keep)_ | `Lenovo` |
| cat-computers.brand | `Dell` | _(Latin — keep)_ | `Dell` |
| cat-computers.brand | `MSI` | _(Latin — keep)_ | `MSI` |
| cat-computers.brand | Прочие | **other** | Other |
| cat-computers.cpu_brand | `Intel` | _(Latin — keep)_ | `Intel` |
| cat-computers.cpu_brand | `AMD` | _(Latin — keep)_ | `AMD` |
| cat-computers.cpu_brand | `Apple` | _(Latin — keep)_ | `Apple` |
| cat-computers.cpu_brand | Другое | **other** | Other |
| cat-consoles.brand | `PlayStation` | _(Latin — keep)_ | `PlayStation` |
| cat-consoles.brand | `Xbox` | _(Latin — keep)_ | `Xbox` |
| cat-consoles.brand | `Nintendo` | _(Latin — keep)_ | `Nintendo` |
| cat-dogs.purpose | Продажа | **sale** | Sale |
| cat-dogs.purpose | Вязка | **mating** | Mating |
| cat-furniture.furn_type | Диваны | **sofas** | Sofas |
| cat-furniture.furn_type | Кровати | **beds** | Beds |
| cat-furniture.furn_type | Шкафы | **wardrobes** | Wardrobes |
| cat-furniture.furn_type | Столы и стулья | **tables-and-chairs** | Tables & chairs |
| cat-furniture.furn_type | Кухни | **kitchens** | Kitchens |
| cat-furniture.furn_type | Детская | **kids** | Kids |
| cat-garages.deal | Продажа | **sale** | Sale |
| cat-garages.deal | Аренда долгосрочная | **long-term-rent** | Long-term rent |
| cat-garages.deal | Аренда посуточно | **daily-rent** | Daily rent |
| cat-garages.garage_type | Гараж | **garage** | Garage |
| cat-garages.garage_type | Парковочное место | **parking** | Parking |
| cat-garages.garage_type | Бокс | **box** | Box |
| cat-houses.deal | Продажа | **sale** | Sale |
| cat-houses.deal | Аренда долгосрочная | **long-term-rent** | Long-term rent |
| cat-houses.deal | Аренда посуточно | **daily-rent** | Daily rent |
| cat-jewelry.acc_type | Часы | **watches** | Watches |
| cat-jewelry.acc_type | Украшения | **jewelry** | Jewelry |
| cat-jewelry.acc_type | Сумки | **bags** | Bags |
| cat-jewelry.acc_type | Очки | **glasses** | Glasses |
| cat-jewelry.acc_type | Прочее | **other** | Other |
| cat-kids.kids_type | Коляски | **strollers** | Strollers |
| cat-kids.kids_type | Автокресла | **car-seats** | Car seats |
| cat-kids.kids_type | Игрушки | **toys** | Toys |
| cat-kids.kids_type | Одежда | **clothing** | Clothing |
| cat-kids.kids_type | Мебель | **furniture** | Furniture |
| cat-kids.age_group | 0–1 | **0-1** | 0–1 |
| cat-kids.age_group | 1–3 | **1-3** | 1–3 |
| cat-kids.age_group | 3–6 | **3-6** | 3–6 |
| cat-kids.age_group | `6+` | _(Latin — keep)_ | `6+` |
| cat-land.deal | Продажа | **sale** | Sale |
| cat-land.deal | Аренда долгосрочная | **long-term-rent** | Long-term rent |
| cat-land.deal | Аренда посуточно | **daily-rent** | Daily rent |
| cat-land.purpose | ИЖС | **residential** | Residential |
| cat-land.purpose | Сельхоз | **agricultural** | Agricultural |
| cat-land.purpose | Коммерческое | **commercial** | Commercial |
| cat-moto.make | `Honda` | _(Latin — keep)_ | `Honda` |
| cat-moto.make | `Yamaha` | _(Latin — keep)_ | `Yamaha` |
| cat-moto.make | `Suzuki` | _(Latin — keep)_ | `Suzuki` |
| cat-moto.make | `Kawasaki` | _(Latin — keep)_ | `Kawasaki` |
| cat-moto.make | `BMW` | _(Latin — keep)_ | `BMW` |
| cat-moto.make | `Racer` | _(Latin — keep)_ | `Racer` |
| cat-moto.make | `Other` | _(Latin — keep)_ | `Other` |
| cat-moto.type | Мотоцикл | **motorcycle** | Motorcycle |
| cat-moto.type | Скутер | **scooter** | Scooter |
| cat-moto.type | Квадроцикл | **atv** | ATV |
| cat-moto.type | Мопед | **moped** | Moped |
| cat-parts.part_type | Шины и диски | **wheels-and-tires** | Wheels & tires |
| cat-parts.part_type | Двигатель | **engine** | Engine |
| cat-parts.part_type | Кузовные | **body** | Body |
| cat-parts.part_type | Электрика | **electrical** | Electrical |
| cat-parts.part_type | Салон | **interior** | Interior |
| cat-parts.part_type | Прочее | **other** | Other |
| cat-pet-supplies.pet_supply | Корма | **food** | Food |
| cat-pet-supplies.pet_supply | Аксессуары | **accessories** | Accessories |
| cat-pet-supplies.pet_supply | Клетки и аквариумы | **cages-and-aquariums** | Cages & aquariums |
| cat-pet-supplies.pet_supply | Уход | **grooming** | Grooming |
| cat-phones.brand | `Apple` | _(Latin — keep)_ | `Apple` |
| cat-phones.brand | `Samsung` | _(Latin — keep)_ | `Samsung` |
| cat-phones.brand | `Xiaomi` | _(Latin — keep)_ | `Xiaomi` |
| cat-phones.brand | `Huawei` | _(Latin — keep)_ | `Huawei` |
| cat-phones.brand | `Honor` | _(Latin — keep)_ | `Honor` |
| cat-phones.brand | `Realme` | _(Latin — keep)_ | `Realme` |
| cat-phones.brand | `OPPO` | _(Latin — keep)_ | `OPPO` |
| cat-phones.brand | `Vivo` | _(Latin — keep)_ | `Vivo` |
| cat-phones.brand | `Nokia` | _(Latin — keep)_ | `Nokia` |
| cat-phones.storage | `32` | _(Latin — keep)_ | `32` |
| cat-phones.storage | `64` | _(Latin — keep)_ | `64` |
| cat-phones.storage | `128` | _(Latin — keep)_ | `128` |
| cat-phones.storage | `256` | _(Latin — keep)_ | `256` |
| cat-phones.storage | `512` | _(Latin — keep)_ | `512` |
| cat-phones.storage | `1024` | _(Latin — keep)_ | `1024` |
| cat-photo.cam_type | Зеркальные | **dslr** | DSLR |
| cat-photo.cam_type | Беззеркальные | **mirrorless** | Mirrorless |
| cat-photo.cam_type | Компактные | **compact** | Compact |
| cat-photo.cam_type | Экшн | **action** | Action |
| cat-shoes.gender | Мужская | **men** | Men |
| cat-shoes.gender | Женская | **women** | Women |
| cat-shoes.gender | Детская | **kids** | Kids |
| cat-shoes.gender | Унисекс | **unisex** | Unisex |
| cat-smartwatch.brand | `Apple` | _(Latin — keep)_ | `Apple` |
| cat-smartwatch.brand | `Samsung` | _(Latin — keep)_ | `Samsung` |
| cat-smartwatch.brand | `Xiaomi` | _(Latin — keep)_ | `Xiaomi` |
| cat-smartwatch.brand | `Huawei` | _(Latin — keep)_ | `Huawei` |
| cat-smartwatch.brand | `Amazfit` | _(Latin — keep)_ | `Amazfit` |
| cat-smartwatch.brand | Другие | **other** | Other |
| cat-tablets.brand | `Apple` | _(Latin — keep)_ | `Apple` |
| cat-tablets.brand | `Samsung` | _(Latin — keep)_ | `Samsung` |
| cat-tablets.brand | `Xiaomi` | _(Latin — keep)_ | `Xiaomi` |
| cat-tablets.brand | `Huawei` | _(Latin — keep)_ | `Huawei` |
| cat-tablets.brand | `Lenovo` | _(Latin — keep)_ | `Lenovo` |
| cat-tablets.brand | Прочие | **other** | Other |
| cat-tablets.storage | `32` | _(Latin — keep)_ | `32` |
| cat-tablets.storage | `64` | _(Latin — keep)_ | `64` |
| cat-tablets.storage | `128` | _(Latin — keep)_ | `128` |
| cat-tablets.storage | `256` | _(Latin — keep)_ | `256` |
| cat-tablets.storage | `512` | _(Latin — keep)_ | `512` |
| cat-tablets.storage | `1024` | _(Latin — keep)_ | `1024` |
| cat-trucks.type | Грузовик | **truck** | Truck |
| cat-trucks.type | Автобус | **bus** | Bus |
| cat-trucks.type | Спецтехника | **machinery** | Machinery |
| cat-trucks.type | Прицеп | **trailer** | Trailer |
| cat-tv.brand | `Samsung` | _(Latin — keep)_ | `Samsung` |
| cat-tv.brand | `LG` | _(Latin — keep)_ | `LG` |
| cat-tv.brand | `Sony` | _(Latin — keep)_ | `Sony` |
| cat-tv.brand | `Xiaomi` | _(Latin — keep)_ | `Xiaomi` |
| cat-tv.brand | `TCL` | _(Latin — keep)_ | `TCL` |
| cat-tv.brand | Прочие | **other** | Other |
| cat-tv.panel | `LED` | _(Latin — keep)_ | `LED` |
| cat-tv.panel | `OLED` | _(Latin — keep)_ | `OLED` |
| cat-tv.panel | `QLED` | _(Latin — keep)_ | `QLED` |