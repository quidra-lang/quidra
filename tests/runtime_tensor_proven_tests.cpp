#include "quidra/abi/runtime_entry_points.hpp"
#include "quidra/abi/tensor_codes.hpp"
#include "quidra/native_extension.h"
#include "runtime_counters.hpp"

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <string_view>
#include <type_traits>
#include <vector>

namespace {
using quidra::counters::Id;
using Entry = decltype(&quidra_tensor_binary);
static_assert(std::is_same_v<Entry,decltype(&quidra_tensor_binary_proven)>);
static_assert(std::is_same_v<decltype(&quidra_tensor_compare),decltype(&quidra_tensor_compare_proven)>);
struct Tensor {
    void* value;
    explicit Tensor(void* raw):value(raw){}
    Tensor(const Tensor&)=delete;
    ~Tensor(){quidra_managed_release(value, reinterpret_cast<void*>(&quidra_tensor_drop));}
};
void require(bool ok,const char* message){if(!ok)throw std::runtime_error(message);}
void* make(const std::vector<long long>& shape,int dtype=QCORE_DTYPE_FLOAT32,int fill=quidra::abi::tensor_fill_mode::ones){
    std::vector<long long> packed{static_cast<long long>(shape.size())};packed.insert(packed.end(),shape.begin(),shape.end());
    return quidra_tensor_create(packed.data(),dtype,fill,false,0,1,1);
}
void values(Tensor& tensor,const std::vector<float>& data){
    require(qcore_tensor_element_count(tensor.value)==data.size(),"fixture element count changed");
    if(!data.empty())std::memcpy(qcore_tensor_cpu_data(tensor.value),data.data(),data.size()*sizeof(float));
}
void same(const Tensor& left,const Tensor& right){
    require(qcore_tensor_dtype(left.value)==qcore_tensor_dtype(right.value) && qcore_tensor_rank(left.value)==qcore_tensor_rank(right.value),"result dtype/rank changed");
    for(std::uint64_t i=0;i<qcore_tensor_rank(left.value);++i)require(qcore_tensor_extent(left.value,i)==qcore_tensor_extent(right.value,i),"result extent changed");
    const auto count=qcore_tensor_element_count(left.value);require(count==qcore_tensor_element_count(right.value),"result count changed");
    const auto bytes=count*(qcore_tensor_dtype(left.value)==QCORE_DTYPE_BOOL?1U:sizeof(float));
    if(bytes)require(std::memcmp(qcore_tensor_cpu_data_const(left.value),qcore_tensor_cpu_data_const(right.value),bytes)==0,"result bits changed");
}
void physical_counts(const std::vector<std::uint64_t>& checked,const std::vector<std::uint64_t>& proven,Id removed){
    require(checked.size()==proven.size(),"counter schema changed");
    for(std::size_t i=0;i<checked.size();++i)if(i!=static_cast<unsigned>(removed))require(checked[i]==proven[i],"non-compatibility work counter changed");
    require(checked[static_cast<unsigned>(removed)]>0 && proven[static_cast<unsigned>(removed)]==0,"compatibility validation was not removed");
    const auto device_checks=checked[static_cast<unsigned>(Id::RuntimeDeviceChecks)];
    // Forward graph construction can check additional operands. All of that
    // mandatory work must remain, rather than requiring one check per entry.
    require(device_checks>0 && proven[static_cast<unsigned>(Id::RuntimeDeviceChecks)]==device_checks,"device checks must remain");
}
void binary_pair(void* left,void* right){
    quidra::counters::reset();Tensor checked(quidra_tensor_binary(left,right,nullptr,quidra::abi::scalar_side::none,quidra::abi::tensor_binary_opcode::add,31,7));
    const auto before=quidra::counters::snapshot();
    quidra::counters::reset();Tensor proven(quidra_tensor_binary_proven(left,right,nullptr,quidra::abi::scalar_side::none,quidra::abi::tensor_binary_opcode::add,31,7));
    const auto after=quidra::counters::snapshot();same(checked,proven);physical_counts(before,after,Id::RuntimeBroadcastValidations);
}
void success(){
    Tensor left(make({2,1})),right(make({1,3}));values(left,{1,2});values(right,{10,20,30});binary_pair(left.value,right.value);
    const std::vector<std::vector<long long>> empty{{0,3},{0,1}};
    for(const auto& shape:empty){Tensor zero(make(shape)),one(make({1,3}));binary_pair(zero.value,one.value);binary_pair(one.value,zero.value);}
    Tensor dense(make({2,3}));values(dense,{1,2,3,4,5,6});Tensor strided(quidra_tensor_transpose(dense.value,0,1,2,1)),row(make({1,2}));values(row,{7,8});binary_pair(strided.value,row.value);
    Tensor tracked_left(quidra_tensor_track(row.value,3,1)),matrix(make({3,2}));binary_pair(tracked_left.value,matrix.value);
    Tensor scalar_left(make({})),scalar_right(make({}));binary_pair(scalar_left.value,scalar_right.value);
    quidra::counters::reset();Tensor mask(quidra_tensor_compare(dense.value,dense.value,nullptr,quidra::abi::scalar_side::none,quidra::abi::tensor_comparison_opcode::equal,32,7));
    const auto checked=quidra::counters::snapshot();
    quidra::counters::reset();Tensor mask_proven(quidra_tensor_compare_proven(dense.value,dense.value,nullptr,quidra::abi::scalar_side::none,quidra::abi::tensor_comparison_opcode::equal,32,7));
    const auto proven=quidra::counters::snapshot();same(mask,mask_proven);physical_counts(checked,proven,Id::RuntimeShapeValidations);
    float scalar=3;
    Tensor scalar_checked(quidra_tensor_binary(dense.value,nullptr,&scalar,quidra::abi::scalar_side::right,quidra::abi::tensor_binary_opcode::multiply,33,7));
    Tensor scalar_proven(quidra_tensor_binary_proven(dense.value,nullptr,&scalar,quidra::abi::scalar_side::right,quidra::abi::tensor_binary_opcode::multiply,33,7));same(scalar_checked,scalar_proven);
    std::puts("tensor proven entries: bitwise values, zero extents, strided/tracked forward and physical counters preserved");
}
void failure(std::string_view kind,bool proven){
    const Entry entry=proven?&quidra_tensor_binary_proven:&quidra_tensor_binary;
    if(kind=="overflow-first"){
        Tensor left(make({2},QCORE_DTYPE_INT8,quidra::abi::tensor_fill_mode::uninitialized)),right(make({2},QCORE_DTYPE_INT8));
        long long index[]{0};std::int8_t maximum=127;quidra_tensor_set(left.value,index,1,&maximum,40,7);
        entry(left.value,right.value,nullptr,quidra::abi::scalar_side::none,quidra::abi::tensor_binary_opcode::add,41,7);
    }else if(kind=="uninitialized"){
        Tensor left(make({2},QCORE_DTYPE_FLOAT32,quidra::abi::tensor_fill_mode::uninitialized)),right(make({2}));
        entry(left.value,right.value,nullptr,quidra::abi::scalar_side::none,quidra::abi::tensor_binary_opcode::divide,41,7);
    }else if(kind=="nonfinite"){
        Tensor left(make({2})),right(make({2}));values(left,{std::numeric_limits<float>::max(),1});values(right,{2,0});
        entry(left.value,right.value,nullptr,quidra::abi::scalar_side::none,quidra::abi::tensor_binary_opcode::multiply,41,7);
    }else if(kind=="dtype"){
        Tensor left(make({2},QCORE_DTYPE_INT8,quidra::abi::tensor_fill_mode::uninitialized)),right(make({2}));
        entry(left.value,right.value,nullptr,quidra::abi::scalar_side::none,quidra::abi::tensor_binary_opcode::add,41,7);
    }else if(kind=="compare-init"){
        Tensor left(make({2},QCORE_DTYPE_FLOAT32,quidra::abi::tensor_fill_mode::uninitialized)),right(make({2}));
        const auto compare=proven?&quidra_tensor_compare_proven:&quidra_tensor_compare;
        compare(left.value,right.value,nullptr,quidra::abi::scalar_side::none,quidra::abi::tensor_comparison_opcode::equal,41,7);
    }else throw std::runtime_error("unknown error case");
    throw std::runtime_error("mandatory tensor failure was bypassed");
}
}
int main(int argc,char** argv){try{
    quidra_runtime_set_args(argc,argv);quidra::counters::set_enabled(true);
    if(argc==4 && std::string_view(argv[1])=="--error"){failure(argv[2],std::string_view(argv[3])=="proven");return 1;}
    require(argc==1,"expected no args or --error CASE checked|proven");success();return 0;
}catch(const std::exception& e){std::fprintf(stderr,"%s\n",e.what());return 1;}}
