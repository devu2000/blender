#include "network.h"

namespace FN {

void MF_EvaluateNetwork::call(const MFMask &mask, MFParams &params, MFContext &context) const
{
  if (mask.indices_amount() == 0) {
    return;
  }

  Storage storage(mask);
  this->copy_inputs_to_storage(params, storage);
  this->evaluate_network_to_compute_outputs(mask, context, storage);
  this->copy_computed_values_to_outputs(mask, params, storage);
}

BLI_NOINLINE void MF_EvaluateNetwork::copy_inputs_to_storage(MFParams &params,
                                                             Storage &storage) const
{
  for (uint i = 0; i < m_inputs.size(); i++) {
    const MFOutputSocket &socket = *m_inputs[i];
    switch (socket.type().category()) {
      case MFDataType::Single: {
        GenericVirtualListRef input_list = params.readonly_single_input(i, "Input");
        for (const MFInputSocket *target : socket.targets()) {
          storage.set_virtual_list_for_input__non_owning(*target, input_list);
        }
        break;
      }
      case MFDataType::Vector: {
        GenericVirtualListListRef input_list_list = params.readonly_vector_input(i, "Input");
        for (const MFInputSocket *target : socket.targets()) {
          const MFNode &target_node = target->node();
          if (target_node.is_function()) {
            const MFFunctionNode &target_function_node = target_node.as_function();
            uint param_index = target_function_node.input_param_indices()[target->index()];
            MFParamType param_type = target_function_node.function().param_type(param_index);

            if (param_type.is_readonly_vector_input()) {
              storage.set_virtual_list_list_for_input__non_owning(*target, input_list_list);
            }
            else if (param_type.is_mutable_vector()) {
              GenericVectorArray *vector_array = new GenericVectorArray(param_type.base_type(),
                                                                        input_list_list.size());
              for (uint i = 0; i < input_list_list.size(); i++) {
                vector_array->extend_single__copy(i, input_list_list[i]);
              }
              storage.set_vector_array_for_input__non_owning(*target, vector_array);
              storage.take_vector_array_ownership(vector_array);
            }
            else {
              BLI_assert(false);
            }
          }
          else {
            storage.set_virtual_list_list_for_input__non_owning(*target, input_list_list);
          }
        }
        break;
      }
    }
  }
}

BLI_NOINLINE void MF_EvaluateNetwork::evaluate_network_to_compute_outputs(
    const MFMask &mask, MFContext &global_context, Storage &storage) const
{
  Stack<const MFSocket *> sockets_to_compute;

  for (const MFInputSocket *input_socket : m_outputs) {
    sockets_to_compute.push(input_socket);
  }

  while (!sockets_to_compute.empty()) {
    const MFSocket &socket = *sockets_to_compute.peek();

    if (socket.is_input()) {
      const MFInputSocket &input_socket = socket.as_input();
      if (storage.input_is_computed(input_socket)) {
        sockets_to_compute.pop();
      }
      else {
        const MFOutputSocket &origin = input_socket.origin();
        sockets_to_compute.push(&origin);
      }
    }
    else {
      const MFOutputSocket &output_socket = socket.as_output();
      const MFFunctionNode &function_node = output_socket.node().as_function();

      uint not_computed_inputs_amount = 0;
      for (const MFInputSocket *input_socket : function_node.inputs()) {
        if (!storage.input_is_computed(*input_socket)) {
          not_computed_inputs_amount++;
          sockets_to_compute.push(input_socket);
        }
      }

      bool all_inputs_are_computed = not_computed_inputs_amount == 0;
      if (all_inputs_are_computed) {
        this->compute_and_forward_outputs(mask, global_context, function_node, storage);
        sockets_to_compute.pop();
      }
    }
  }
}

BLI_NOINLINE void MF_EvaluateNetwork::compute_and_forward_outputs(
    const MFMask &mask,
    MFContext &global_context,
    const MFFunctionNode &function_node,
    Storage &storage) const
{
  uint array_size = mask.min_array_size();

  const MultiFunction &function = function_node.function();
  MFParamsBuilder params_builder(function, array_size);

  Vector<std::pair<const MFOutputSocket *, GenericMutableArrayRef>> single_outputs_to_forward;
  Vector<std::pair<const MFOutputSocket *, GenericVectorArray *>> vector_outputs_to_forward;

  for (uint param_index : function.param_indices()) {
    MFParamType param_type = function.param_type(param_index);
    switch (param_type.category()) {
      case MFParamType::ReadonlySingleInput: {
        uint input_socket_index = function_node.input_param_indices().first_index(param_index);
        const MFInputSocket &input_socket = *function_node.inputs()[input_socket_index];
        GenericVirtualListRef values = storage.get_virtual_list_for_input(input_socket);
        params_builder.add_readonly_single_input(values);
        break;
      }
      case MFParamType::ReadonlyVectorInput: {
        uint input_socket_index = function_node.input_param_indices().first_index(param_index);
        const MFInputSocket &input_socket = *function_node.inputs()[input_socket_index];
        GenericVirtualListListRef values = storage.get_virtual_list_list_for_input(input_socket);
        params_builder.add_readonly_vector_input(values);
        break;
      }
      case MFParamType::SingleOutput: {
        uint output_socket_index = function_node.output_param_indices().first_index(param_index);
        const MFOutputSocket &output_socket = *function_node.outputs()[output_socket_index];
        GenericMutableArrayRef values_destination = this->allocate_array(
            output_socket.type().type(), array_size);
        params_builder.add_single_output(values_destination);
        single_outputs_to_forward.append({&output_socket, values_destination});
        break;
      }
      case MFParamType::VectorOutput: {
        uint output_socket_index = function_node.output_param_indices().first_index(param_index);
        const MFOutputSocket &output_socket = *function_node.outputs()[output_socket_index];
        auto *values_destination = new GenericVectorArray(output_socket.type().base_type(),
                                                          array_size);
        params_builder.add_vector_output(*values_destination);
        vector_outputs_to_forward.append({&output_socket, values_destination});
        break;
      }
      case MFParamType::MutableVector: {
        uint input_socket_index = function_node.input_param_indices().first_index(param_index);
        const MFInputSocket &input_socket = *function_node.inputs()[input_socket_index];

        uint output_socket_index = function_node.output_param_indices().first_index(param_index);
        const MFOutputSocket &output_socket = *function_node.outputs()[output_socket_index];

        GenericVectorArray &values = storage.get_vector_array_for_input(input_socket);
        params_builder.add_mutable_vector(values);
        vector_outputs_to_forward.append({&output_socket, &values});
        break;
      }
    }
  }

  MFParams &params = params_builder.build();
  function.call(mask, params, global_context);

  for (auto single_forward_info : single_outputs_to_forward) {
    const MFOutputSocket &output_socket = *single_forward_info.first;
    GenericMutableArrayRef values = single_forward_info.second;
    storage.take_array_ref_ownership(values);

    for (const MFInputSocket *target : output_socket.targets()) {
      storage.set_virtual_list_for_input__non_owning(*target, values);
    }
  }

  for (auto vector_forward_info : vector_outputs_to_forward) {
    const MFOutputSocket &output_socket = *vector_forward_info.first;
    GenericVectorArray *values = vector_forward_info.second;
    storage.take_vector_array_ownership__not_twice(values);

    for (const MFInputSocket *target : output_socket.targets()) {
      const MFNode &target_node = target->node();
      if (target_node.is_function()) {
        const MFFunctionNode &target_function_node = target_node.as_function();
        uint param_index = target_function_node.input_param_indices()[target->index()];
        MFParamType param_type = target_function_node.function().param_type(param_index);

        if (param_type.is_readonly_vector_input()) {
          storage.set_virtual_list_list_for_input__non_owning(*target, *values);
        }
        else if (param_type.is_mutable_vector()) {
          GenericVectorArray *copied_values = new GenericVectorArray(values->type(),
                                                                     values->size());
          for (uint i = 0; i < values->size(); i++) {
            copied_values->extend_single__copy(i, (*values)[i]);
          }
          storage.take_vector_array_ownership(copied_values);
          storage.set_vector_array_for_input__non_owning(*target, copied_values);
        }
        else {
          BLI_assert(false);
        }
      }
      else if (m_outputs.contains(target)) {
        storage.set_virtual_list_list_for_input__non_owning(*target, *values);
      }
    }
  }
}

BLI_NOINLINE void MF_EvaluateNetwork::copy_computed_values_to_outputs(const MFMask &mask,
                                                                      MFParams &params,
                                                                      Storage &storage) const
{
  for (uint output_index = 0; output_index < m_outputs.size(); output_index++) {
    uint global_param_index = m_inputs.size() + output_index;
    const MFInputSocket &socket = *m_outputs[output_index];
    switch (socket.type().category()) {
      case MFDataType::Single: {
        GenericVirtualListRef values = storage.get_virtual_list_for_input(socket);
        GenericMutableArrayRef output_values = params.single_output(global_param_index, "Output");
        for (uint i : mask.indices()) {
          output_values.copy_in__uninitialized(i, values[i]);
        }
        break;
      }
      case MFDataType::Vector: {
        GenericVirtualListListRef values = storage.get_virtual_list_list_for_input(socket);
        GenericVectorArray &output_values = params.vector_output(global_param_index, "Output");
        for (uint i : mask.indices()) {
          output_values.extend_single__copy(i, values[i]);
        }
        break;
      }
    }
  }
}

}  // namespace FN