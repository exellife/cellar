# Transport [Транспорт]  ·  `cat-transport`

<!-- KY display = the [bracket] where present, else the English word. Bare = same in both (SUV, Toyota…). -->

## Subcategories
- **Cars** [Унаалар] · `cat-cars`
- **Motorcycles** [Мотоциклдер] · `cat-moto`
- **Trucks & machinery** [Жүк унаа] · `cat-trucks`
- **Parts & accessories** [Тетиктер] · `cat-parts`

## Cars · `cat-cars`
- **Make** [Марка] `make` (enum): Toyota · Honda · Nissan · Mercedes-Benz · BMW · Audi · Lexus · Hyundai · Kia · Lada · Mitsubishi · Volkswagen
- **Model** [Модел] `model` (text)
- **Year** [Жылы] `year` (int)
- **Mileage** [Жүрүшү] `mileage` (int, km)
- **Transmission** [Трансмиссия] `transmission` (enum): Automatic [Автомат] · Manual [Механика] · CVT [Вариатор] · AMT [Робот]
- **Fuel** [Отун] `fuel` (enum): Petrol [Бензин] · Diesel [Дизел] · Gas [Газ] · Hybrid [Гибрид] · Electric [Электр]
- **Body type** [Денеси] `body` (enum): Sedan · Hatchback · Wagon · SUV · Minivan · Coupe · Pickup
- **Drivetrain** [Жетек] `drive` (enum): FWD [FWD (Алдыңкы)] · RWD [RWD (Арткы)] · 4x4 [4x4 (Толук)]
- **Engine** [Мотор] `engine_l` (number, L)

## Motorcycles · `cat-moto`
- **Make** [Марка] `make` (enum): Honda · Yamaha · Suzuki · Kawasaki · BMW · Racer · Other [Башка]
- **Year** [Жылы] `year` (int)
- **Engine** [Мотор] `engine_cc` (int, cc)
- **Type** [Түрү] `type` (enum): Motorcycle [Мотоцикл] · Scooter [Скутер] · ATV · Moped [Мопед]

## Trucks & machinery · `cat-trucks`
- **Type** [Түрү] `type` (enum): Truck [Жүк унаа] · Bus [Автобус] · Machinery [Техника] · Trailer [Чиркегич]
- **Year** [Жылы] `year` (int)
- **Mileage** [Жүрүшү] `mileage` (int, km)

## Parts & accessories · `cat-parts`
- **Category** [Түрү] `part_type` (enum): Wheels & tires [Дөңгөлөктөр] · Engine [Мотор] · Body [Кузов бөлүктөрү] · Electrical [Электрика] · Interior [Салон] · Other [Башка]
