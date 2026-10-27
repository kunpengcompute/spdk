#include <pthread.h>
typedef pthread_mutex_t pthread_spinlock_t;
static inline int pthread_spin_init(pthread_spinlock_t *lock, int scope) {
 return pthread_mutex_init(lock, NULL);
}
#define pthread_spin_lock pthread_mutex_lock
#define pthread_spin_unlock pthread_mutex_unlock
#define pthread_spin_destroy pthread_mutex_destroy
