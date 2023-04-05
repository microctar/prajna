#pragma once

#include <list>
#include <set>
#include <unordered_map>

#include "prajna/ir/ir.hpp"
#include "prajna/logger.hpp"
#include "prajna/lowering/statement_lowering_visitor.hpp"
#include "prajna/parser/parse.h"
#include "prajna/transform/transform_pass.hpp"
#include "prajna/transform/utility.hpp"

namespace prajna::transform {

/// @note 删除的节点需要调用finilize函数, 以避免循环依赖导致的内存泄露
/// @note 当遍历某个操作时, 先拷贝一下, 除非确定没有插入删除操作

/// @brief 将变量转换为指针的存取
class ConvertVariableToPointerPass : public FunctionPass {
   public:
    bool insertValueToBlock(std::shared_ptr<ir::Function> ir_function) {
        bool changed = false;
        std::set<std::shared_ptr<ir::Value>> ir_values;
        for (auto ir_block : ir_function->blocks) {
            for (auto iter_ir_value = ir_block->values.begin();
                 iter_ir_value != ir_block->values.end(); ++iter_ir_value) {
                auto ir_value = *iter_ir_value;
                ir_values.insert(ir_value);

                if (auto ir_instruction = cast<ir::Instruction>(ir_value)) {
                    for (auto i = 0; i < ir_instruction->operandSize(); ++i) {
                        auto ir_operand = ir_instruction->operand(i);
                        if (ir_values.count(ir_operand) == 0) {
                            // 模板实例化时会产生非Block的Constant,并且会被使用
                            if (is<ir::ConstantInt>(ir_operand)) {
                                ir_block->insert(iter_ir_value, ir_operand);
                                ir_values.insert(ir_operand);
                                changed = true;
                            }
                        }
                    }
                }
            }
        }

        return changed;
    }

    bool convertThisWrapperToDeferencePointer(std::shared_ptr<ir::Function> ir_function) {
        auto ir_this_wrappers = utility::getValuesInFunction<ir::ThisWrapper>(ir_function);

        // 可能需要重构, 应为"this;"这样的代码无意义, 也许应该取个右值.
        PRAJNA_ASSERT(ir_this_wrappers.size() <= 1, "只可能成员函数开头加入一个");
        for (auto ir_this_wrapper : ir_this_wrappers) {
            // 改变使用它的
            auto instructions_with_index_copy = ir_this_wrapper->instruction_with_index_list;
            for (auto instruction_with_index_list : instructions_with_index_copy) {
                auto ir_inst = instruction_with_index_list.instruction;
                size_t op_idx = instruction_with_index_list.operand_index;
                auto ir_deference_pointer =
                    ir::DeferencePointer::create(ir_this_wrapper->thisPointer());
                auto ir_block = ir_inst->parent_block;
                auto iter = std::find(ir_block->values.begin(), ir_block->values.end(), ir_inst);
                ir_block->insert(iter, ir_deference_pointer);
                ir_inst->operand(op_idx, ir_deference_pointer);
            }

            ir_this_wrapper->parent_block->remove(ir_this_wrapper);
            ir_this_wrapper->finalize();
        }

        return !ir_this_wrappers.empty();
    }

    bool convertVariableToDeferencePointer(std::shared_ptr<ir::Function> ir_function) {
        auto ir_variables = utility::getValuesInFunction<ir::LocalVariable>(ir_function);

        for (auto ir_variable : ir_variables) {
            //改变它自己
            std::shared_ptr<ir::Value> ir_alloca = ir::Alloca::create(ir_variable->type);
            ir_alloca->name = ir_variable->name;
            ir_alloca->fullname = ir_alloca->name;

            // @note ir::Alloca不应该出现在循环体内部, 故直接再将其放在函数的第一个块
            ir_function->blocks.front()->pushFront(ir_alloca);
            ir_variable->parent_block->remove(ir_variable);

            // 改变使用它的
            auto instructions_with_index_copy = ir_variable->instruction_with_index_list;
            for (auto instruction_with_index_list : instructions_with_index_copy) {
                auto ir_inst = instruction_with_index_list.instruction;
                size_t op_idx = instruction_with_index_list.operand_index;

                auto ir_deference_pointer = ir::DeferencePointer::create(ir_alloca);
                auto ir_block = ir_inst->parent_block;
                auto iter = std::find(ir_block->values.begin(), ir_block->values.end(), ir_inst);
                ir_block->insert(iter, ir_deference_pointer);
                ir_inst->operand(op_idx, ir_deference_pointer);
            }

            ir_variable->finalize();
        }

        return !ir_variables.empty();
    }

    bool convertAccessFieldToGetStructElementPointer(std::shared_ptr<ir::Function> ir_function) {
        auto ir_access_fields = utility::getValuesInFunction<ir::AccessField>(ir_function);

        for (auto ir_access_field : ir_access_fields) {
            auto ir_object_deference_ptr = cast<ir::DeferencePointer>(ir_access_field->object());
            PRAJNA_ASSERT(ir_object_deference_ptr);
            auto ir_struct_get_element_ptr = ir::GetStructElementPointer::create(
                ir_object_deference_ptr->pointer(), ir_access_field->field);
            PRAJNA_ASSERT(ir_object_deference_ptr->parent_block,
                          "解指针可能被多次删除, 不符合使用场景");
            ir_object_deference_ptr->parent_block->remove(ir_object_deference_ptr);
            ir_object_deference_ptr->finalize();

            auto instructions_with_index_copy = ir_access_field->instruction_with_index_list;
            for (auto instruction_with_index_list : instructions_with_index_copy) {
                auto ir_inst = instruction_with_index_list.instruction;
                size_t op_idx = instruction_with_index_list.operand_index;

                auto ir_deference_pointer = ir::DeferencePointer::create(ir_struct_get_element_ptr);
                auto iter_inst = std::find(RANGE(ir_inst->parent_block->values), ir_inst);
                ir_inst->parent_block->insert(iter_inst, ir_deference_pointer);
                ir_inst->operand(op_idx, ir_deference_pointer);
            }

            utility::replaceInBlock(ir_access_field, ir_struct_get_element_ptr);
            ir_access_field->finalize();
        }

        return !ir_access_fields.empty();
    }

    bool convertIndexArrayToGetArrayElementPointer(std::shared_ptr<ir::Function> ir_function) {
        auto ir_index_arrays = utility::getValuesInFunction<ir::IndexArray>(ir_function);

        for (auto ir_index_array : ir_index_arrays) {
            auto ir_object_deference_ptr = cast<ir::DeferencePointer>(ir_index_array->object());
            PRAJNA_ASSERT(ir_object_deference_ptr);
            auto ir_array_get_element_ptr = ir::GetArrayElementPointer::create(
                ir_object_deference_ptr->pointer(), ir_index_array->index());
            ir_object_deference_ptr->parent_block->remove(ir_object_deference_ptr);
            ir_object_deference_ptr->finalize();

            auto instructions_with_index_copy = ir_index_array->instruction_with_index_list;
            for (auto instruction_with_index_list : instructions_with_index_copy) {
                auto ir_inst = instruction_with_index_list.instruction;
                size_t op_idx = instruction_with_index_list.operand_index;

                auto ir_deference_pointer = ir::DeferencePointer::create(ir_array_get_element_ptr);
                auto iter_inst = std::find(RANGE(ir_inst->parent_block->values), ir_inst);
                ir_inst->parent_block->insert(iter_inst, ir_deference_pointer);
                ir_inst->operand(op_idx, ir_deference_pointer);
            }

            utility::replaceInBlock(ir_index_array, ir_array_get_element_ptr);
            ir_index_array->finalize();
        }

        return !ir_index_arrays.empty();
    }

    bool convertIndexPointerToGetPointerElementPointer(std::shared_ptr<ir::Function> ir_function) {
        auto ir_index_pointers = utility::getValuesInFunction<ir::IndexPointer>(ir_function);

        for (auto ir_index_pointer : ir_index_pointers) {
            auto ir_object_deference_ptr = cast<ir::DeferencePointer>(ir_index_pointer->object());
            PRAJNA_ASSERT(ir_object_deference_ptr);
            auto ir_pointer_get_element_ptr = ir::GetPointerElementPointer::create(
                ir_object_deference_ptr->pointer(), ir_index_pointer->index());
            ir_object_deference_ptr->parent_block->remove(ir_object_deference_ptr);
            ir_object_deference_ptr->finalize();

            auto instructions_with_index_copy = ir_index_pointer->instruction_with_index_list;
            for (auto instruction_with_index_list : instructions_with_index_copy) {
                auto ir_inst = instruction_with_index_list.instruction;
                size_t op_idx = instruction_with_index_list.operand_index;

                auto ir_deference_pointer =
                    ir::DeferencePointer::create(ir_pointer_get_element_ptr);
                auto iter_inst = std::find(RANGE(ir_inst->parent_block->values), ir_inst);
                ir_inst->parent_block->insert(iter_inst, ir_deference_pointer);
                ir_inst->operand(op_idx, ir_deference_pointer);
            }

            utility::replaceInBlock(ir_index_pointer, ir_pointer_get_element_ptr);
            ir_index_pointer->finalize();
        }

        return !ir_index_pointers.empty();
    }

    bool convertGetAddressOfVaraibleLikedToPointer(std::shared_ptr<ir::Function> ir_function) {
        auto ir_get_addresses =
            utility::getValuesInFunction<ir::GetAddressOfVariableLiked>(ir_function);

        for (auto ir_get_address : ir_get_addresses) {
            auto instructions_with_index_copy = ir_get_address->instruction_with_index_list;
            for (auto instruction_with_index_list : instructions_with_index_copy) {
                auto ir_inst = instruction_with_index_list.instruction;
                size_t op_idx = instruction_with_index_list.operand_index;

                auto ir_deference_pointer = cast<ir::DeferencePointer>(ir_get_address->variable());
                PRAJNA_ASSERT(ir_deference_pointer,
                              "需要将VariableLiked都转换为DeferencePoitner后再执行该操作");
                ir_deference_pointer->pointer();
                ir_inst->operand(op_idx, ir_deference_pointer->pointer());
            }

            ir_get_address->parent_block->remove(ir_get_address);
            ir_get_address->finalize();
        }

        return !ir_get_addresses.empty();
    }

    bool convertDeferencePointerToStoreAndLoadPointer(std::shared_ptr<ir::Function> ir_function) {
        auto ir_deference_pointers =
            utility::getValuesInFunction<ir::DeferencePointer>(ir_function);

        for (auto ir_deference_pointer : ir_deference_pointers) {
            auto ir_pointer = ir_deference_pointer->pointer();
            auto iter_deference_pointer =
                ir_deference_pointer->parent_block->find(ir_deference_pointer);
            auto instructions_with_index_copy = ir_deference_pointer->instruction_with_index_list;
            for (auto instruction_with_index : instructions_with_index_copy) {
                auto ir_inst = instruction_with_index.instruction;
                size_t op_idx = instruction_with_index.operand_index;

                if (is<ir::WriteVariableLiked>(ir_inst) && op_idx == 1) {
                    auto ir_write_variable_liked = cast<ir::WriteVariableLiked>(ir_inst);
                    auto ir_store =
                        ir::StorePointer::create(ir_write_variable_liked->value(), ir_pointer);
                    utility::replaceInBlock(ir_write_variable_liked, ir_store);
                } else {
                    auto ir_load = ir::LoadPointer::create(ir_pointer);
                    ir_deference_pointer->parent_block->insert(iter_deference_pointer, ir_load);
                    ir_inst->operand(op_idx, ir_load);
                }
            }
            ir_deference_pointer->parent_block->erase(iter_deference_pointer);
            ir_deference_pointer->finalize();
        }

        return !ir_deference_pointers.empty();
    }

    bool runOnFunction(std::shared_ptr<ir::Function> ir_function) override {
        bool changed = false;
        changed = this->insertValueToBlock(ir_function) || changed;
        changed = this->convertThisWrapperToDeferencePointer(ir_function) || changed;
        changed = this->convertVariableToDeferencePointer(ir_function) || changed;
        changed = this->convertAccessFieldToGetStructElementPointer(ir_function) || changed;
        changed = this->convertIndexArrayToGetArrayElementPointer(ir_function) || changed;
        changed = this->convertIndexPointerToGetPointerElementPointer(ir_function) || changed;
        // 需要把VariableLiked都转换完了后再执行
        changed = this->convertGetAddressOfVaraibleLikedToPointer(ir_function) || changed;
        changed = this->convertDeferencePointerToStoreAndLoadPointer(ir_function) || changed;

        return changed;
    }
};

}  // namespace prajna::transform
