// Apply one decoded message to any book with the GoldenBook/FastBook API.
#pragma once

#include "fh/parser/message.hpp"

namespace fh {

template <class Book>
inline void apply(Book& book, const Msg& m) {
    switch (m.type) {
        case MsgType::AddOrder:
        case MsgType::AddOrderMpid:
            book.add(m.ref, m.locate, m.side, m.price, m.shares);
            break;
        case MsgType::OrderExecuted:
        case MsgType::OrderExecutedPrice:  // book effect is the same as E
            book.execute(m.ref, m.shares);
            break;
        case MsgType::OrderCancel:
            book.cancel(m.ref, m.shares);
            break;
        case MsgType::OrderDelete:
            book.remove(m.ref);
            break;
        case MsgType::OrderReplace:
            book.replace(m.ref, m.new_ref, m.price, m.shares);
            break;
        case MsgType::StockDirectory:
            book.directory(m.locate, m.symbol);
            break;
        default:
            break;
    }
}

}  // namespace fh
