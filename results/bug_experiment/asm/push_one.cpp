#include "fh/queue/spsc_queue.hpp"
#include "fh/parser/message.hpp"
// One push into a 64-slot ring: the slot store, then the head publish.
bool push_one(fh::SpscQueue<fh::Msg, 64>& q, const fh::Msg& m) { return q.try_push(m); }
