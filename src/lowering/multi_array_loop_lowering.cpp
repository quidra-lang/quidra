#include "lowering/checked_types.hpp"
#include "lowering/lowerer.hpp"

#include <string>
#include <vector>

namespace quidra::lowering {

void ControlFlowLowering::lower_multi_array_for(const ForStmt& loop) {
    const auto integer=Type::simple(TypeKind::Int64);
    const auto boolean=Type::simple(TypeKind::Bool);
    struct Collection {
        const Expr* expression;
        std::string name;
        bool declares;
        LifetimeLowering::LoopSource snapshot;
        ValueId length;
        bool initialized;
        std::string local;
    };
    std::vector<Collection> collections;
    const auto snapshot=[&](const Expr& expression,const std::string& name,bool declares) {
        const auto type=type_of(checked_,expression);
        const auto original=lowerer_.lower(expression);
        // Copy now, before a later header expression can modify this source.
        auto copy=lifetime_.copy_value(original,type);
        lifetime_.release_temporary(expression,original);
        LifetimeLowering::LoopSource source{
            builder_.hidden("for.zip.source"),builder_.hidden("for.zip.owned"),type};
        scope_.local_type(source.source)=type;
        scope_.local_type(source.owned)=boolean;
        builder_.emit(StoreLocal{source.source,copy,type,true});
        builder_.emit(StoreLocal{source.owned,builder_.const_bool(true),boolean});
        lifetime_.enter_loop_source(source);
        auto length=builder_.fresh();
        builder_.emit(ArrayLength{length,copy});
        collections.push_back({&expression,name,declares,std::move(source),length,
            facts_.array_initialization.fully_initialized(expression),{}});
    };
    snapshot(*loop.iterable,loop.name,loop.declares);
    for(const auto& binding:loop.collections)
        snapshot(*binding.iterable,binding.name,binding.declares);
    // No binding or body effect occurs until every source has been evaluated
    // and every length has been checked against the first snapshot.
    const auto origin=loop.iterable->span.start;
    for(std::size_t i=1;i<collections.size();++i)
        builder_.emit(ExtentEqualCheck{collections[i].length,collections.front().length,
            {static_cast<std::uint32_t>(origin.line),
            static_cast<std::uint32_t>(origin.column)},true});
    for(auto& collection:collections) {
        if(collection.name=="_") continue;
        const auto item=*collection.snapshot.type.first;
        collection.local=collection.declares
            ?scope_.bind_source_local(builder_,collection.name,item)
            :scope_.source_local(collection.name);
    }
    const auto index_name=builder_.hidden("for.zip.index");
    scope_.local_type(index_name)=integer;
    builder_.emit(StoreLocal{index_name,builder_.const_int(0),integer});
    const auto condition=builder_.label("for.zip.cond");
    const auto body=builder_.label("for.zip.body");
    const auto advance=builder_.label("for.zip.advance");
    const auto done=builder_.label("for.zip.end");
    builder_.emit(Jump{condition});
    builder_.enter(builder_.add_block(condition));
    auto index=builder_.fresh(),more=builder_.fresh();
    builder_.emit(LoadLocal{index,index_name,integer});
    builder_.emit(Binary{more,"<",index,collections.front().length,integer,boolean});
    builder_.emit(Branch{more,body,done});
    builder_.enter(builder_.add_block(body));
    for(const auto& collection:collections) {
        if(collection.name=="_") continue;
        const auto item=*collection.snapshot.type.first;
        auto array=builder_.fresh(),element=builder_.fresh();
        builder_.emit(LoadLocal{array,collection.snapshot.source,collection.snapshot.type});
        const auto start=collection.expression->span.start;
        // Snapshot copies preserve initialized storage, but unknown array
        // initialization is still checked by ArrayGet before publishing it.
        builder_.emit(ArrayGet{element,array,index,item,
            {static_cast<std::uint32_t>(start.line),static_cast<std::uint32_t>(start.column)},
            collection.initialized,true});
        element=lifetime_.copy_value(element,item);
        builder_.emit(StoreLocal{collection.local,element,item});
    }
    lowerer_.initialization_flags().for_iteration(loop);
    loop_targets_.enter(advance,done);
    lowerer_.lower_block(loop.body);
    loop_targets_.leave();
    if(!current_block_ended(builder_)) builder_.emit(Jump{advance});
    builder_.enter(builder_.add_block(advance));
    auto old=builder_.fresh(),next=builder_.fresh();
    builder_.emit(LoadLocal{old,index_name,integer});
    // index < length implies index + 1 <= length, within the ABI i64 length.
    builder_.emit(Binary{next,"+",old,builder_.const_int(1),integer,integer,0,0,true});
    builder_.emit(StoreLocal{index_name,next,integer});
    builder_.emit(Jump{condition});
    builder_.enter(builder_.add_block(done));
    for(std::size_t i=collections.size();i>0;--i) {
        lifetime_.leave_loop_source();
        lifetime_.release_loop_source(collections[i-1].snapshot);
    }
}

} // namespace quidra::lowering
