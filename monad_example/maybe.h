#ifndef MAYBE_H
#define MAYBE_H 1

#include "../include/tc_common_macros.h"
#include "../include/typeclass.h"
#include "monad.h"
#include "functor.h"

TC_BEGIN_DECLS

enum maybe_e {
  MAYBE_JUST,
  MAYBE_NOTHING,
};

typedef struct Maybe {
  struct TCObject;
  enum maybe_e maybe_e;
  union {
    void* data;
    void (*apply)(void*, void**);
  };
} Maybe;

void just(Maybe* self, void* data);
void nothing(Maybe* self);

int Maybe_m_return(TCObject* _self, void* data);
int Maybe_m_bind(TCObject* _self, m_bind_callback cb, TCObject* _next);
int Maybe_m_then(TCObject* self, TCObject* next);
int Maybe_f_fmap(TCObject* self, TCObject* next, f_fmap_callback cb);
int Maybe_a_pure(TCObject* self, void* data);
int Maybe_a_ap(TCObject* self, TCObject* a, TCObject* b);

TC_END_DECLS

#endif /* MAYBE_H */