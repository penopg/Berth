// Чтение JSON вида «массив объектов с плоскими полями».
//
// Ровно под projects.json и ничего сверх: полноценный парсер здесь не нужен,
// а лишняя зависимость в сборке стоит дороже, чем полторы сотни строк.
// Вложенные объекты и массивы внутри элемента пропускаются, не разбираясь.
#ifndef BERTH_JSON_H
#define BERTH_JSON_H

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    const char *p;
    const char *end;
} JsonScan;

// Готовит обход. Ожидает, что документ — массив. false, если это не так.
bool json_scan_init(JsonScan *s, const char *text, size_t len);

// Переходит к следующему объекту массива. false — массив кончился.
bool json_scan_object(JsonScan *s);

// Следующее поле текущего объекта. Значения не-строк пропускаются.
// false — поля кончились.
bool json_scan_field(JsonScan *s, char *key, size_t key_cap,
                     char *val, size_t val_cap);

#endif // BERTH_JSON_H
