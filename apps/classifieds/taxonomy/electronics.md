# Electronics [Электроника]  ·  `cat-electronics`

## Subcategories
- **Phones** [Телефондор] · `cat-phones`
- **Computers** [Компютерлер] · `cat-computers`
- **Tablets** [Планшеттер] · `cat-tablets`
- **TV & projectors** [ТВ жана проекторлор] · `cat-tv`
- **Audio** [Аудио] · `cat-audio`
- **Photo & video** [Фото жана видео] · `cat-photo`
- **Consoles** [Оюн консолдору] · `cat-consoles`
- **Smartwatches** [Акылдуу сааттар] · `cat-smartwatch`

## Phones · `cat-phones`
- **Brand** [Бренд] `brand` (enum): Apple · Samsung · Xiaomi · Huawei · Honor · Realme · OPPO · Vivo · Nokia
- **Storage** `storage` (enum, GB): 32 · 64 · 128 · 256 · 512 · 1024
- **Model** [Модел] `model` (text)

## Computers · `cat-computers`
- **Type** [Түрү] `kind` (enum): Laptop · All-in-One · Desktop · Components
- **RAM** `ram` (enum, GB): 4 · 8 · 16 · 32 · 64
- **Brand** [Бренд] `brand` (enum): Apple · Asus · Acer · HP · Lenovo · Dell · MSI · Other [Башка]
- **Storage** `storage_gb` (int, GB)
- **CPU** `cpu_brand` (enum): Intel · AMD · Apple · Other [Башка]
- **CPU model** `cpu_model` (text)

## Tablets · `cat-tablets`
- **Brand** [Бренд] `brand` (enum): Apple · Samsung · Xiaomi · Huawei · Lenovo · Other [Башка]
- **Storage** `storage` (enum, GB): 32 · 64 · 128 · 256 · 512 · 1024

## TV & projectors · `cat-tv`
- **Brand** [Бренд] `brand` (enum): Samsung · LG · Sony · Xiaomi · TCL · Other [Башка]
- **Screen size** [Диагонал] `screen_in` (int, inch)
- **Panel** `panel` (enum): LED · OLED · QLED

## Audio · `cat-audio`
- **Type** [Түрү] `audio_type` (enum): Headphones [Кулакчындар] · Speakers [Колонкалар] · Soundbar · Microphones [Микрофондор]

## Photo & video · `cat-photo`
- **Type** [Түрү] `cam_type` (enum): DSLR · Mirrorless · Compact · Action

## Consoles · `cat-consoles`
- **Brand** [Бренд] `brand` (enum): PlayStation · Xbox · Nintendo

## Smartwatches · `cat-smartwatch`
- **Brand** [Бренд] `brand` (enum): Apple · Samsung · Xiaomi · Huawei · Amazfit · Other [Башка]
