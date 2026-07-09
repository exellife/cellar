# Real Estate [Кыймылсыз мүлк]  ·  `cat-realestate`

## Subcategories
- **Apartments** [Батирлер] · `cat-apartments`
- **Houses** [Үйлөр] · `cat-houses`
- **Commercial** [Коммерциялык] · `cat-commercial`
- **Land** [Жер тилкелери] · `cat-land`
- **Garages** [Гараждар] · `cat-garages`

## Apartments · `cat-apartments`
- **Listing type** [Бүтүм түрү] `deal` (enum): Sale [Сатуу] · Long-term rent [Узак ижара] · Daily rent [Күндүк ижара]
- **Rooms** [Бөлмө] `rooms` (int)
- **Area** [Аянты] `area` (number, m²)
- **Floor** [Кабат] `floor` (int)
- **Total floors** [Кабаттуулугу] `total_floors` (int)
- **Furnished** [Эмерек] `furnished` (bool)

## Houses · `cat-houses`
- **Listing type** [Бүтүм түрү] `deal` (enum): Sale [Сатуу] · Long-term rent [Узак ижара] · Daily rent [Күндүк ижара]
- **Rooms** [Бөлмө] `rooms` (int)
- **Area** [Аянты] `area` (number, m²)
- **Land** [Тилке] `land` (number, sotka)

## Commercial · `cat-commercial`
- **Listing type** [Бүтүм түрү] `deal` (enum): Sale [Сатуу] · Long-term rent [Узак ижара] · Daily rent [Күндүк ижара]
- **Type** [Түрү] `comm_type` (enum): Office [Офис] · Shop [Дүкөн] · Warehouse [Кампа] · Production [Өндүрүш] · Catering [Коомдук тамактануу]
- **Area** [Аянты] `area` (number, m²)

## Land · `cat-land`
- **Listing type** [Бүтүм түрү] `deal` (enum): Sale [Сатуу] · Long-term rent [Узак ижара] · Daily rent [Күндүк ижара]
- **Area** [Аянты] `land` (number, sotka)
- **Purpose** [Багыты] `purpose` (enum): Residential [Турак жай] · Agricultural [Айыл чарба] · Commercial [Коммерциялык]

## Garages · `cat-garages`
- **Listing type** [Бүтүм түрү] `deal` (enum): Sale [Сатуу] · Long-term rent [Узак ижара] · Daily rent [Күндүк ижара]
- **Type** [Түрү] `garage_type` (enum): Garage [Гараж] · Parking [Паркинг] · Box [Бокс]
