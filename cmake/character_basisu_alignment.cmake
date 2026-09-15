# Local defensive amendment to the hash-verified BasisU transcoder. Byte
# buffers need not have the alignment of the decoder's internal block type.
# Keep every shipped output format on an aligned local copy, without changing
# block contents, allocation limits, or the upstream decoder algorithms.
function(mdkr_basisu_align_blocks source)
    file(READ "${source}" contents)
    set(old_pointer "const uastc_block* pSource_block = reinterpret_cast<const uastc_block *>(pImage_data);")
    set(new_pointer "const uint8_t* pSource_bytes = pImage_data;")
    set(old_loop [=[for (uint32_t block_x = 0; block_x < num_blocks_x; ++block_x, ++pSource_block, pDst_block = (uint8_t *)pDst_block + output_block_or_pixel_stride_in_bytes)
				{
					switch (fmt)]=])
    set(new_loop [=[for (uint32_t block_x = 0; block_x < num_blocks_x; ++block_x, pSource_bytes += sizeof(uastc_block), pDst_block = (uint8_t *)pDst_block + output_block_or_pixel_stride_in_bytes)
				{
					// MDKR local amendment: input is byte-addressed, not an aligned C++ object.
					uastc_block source_block;
					memcpy(&source_block, pSource_bytes, sizeof(source_block));
					const uastc_block* pSource_block = &source_block;
					switch (fmt)]=])
    # Fail closed if an upstream update changes either patch site. Downloads
    # are independently SHA-256 checked before this function is called.
    foreach(site IN ITEMS pointer loop)
        string(FIND "${contents}" "${old_${site}}" position)
        if(position EQUAL -1)
            message(FATAL_ERROR "BasisU alignment amendment no longer matches: ${site}")
        endif()
        string(REPLACE "${old_${site}}" "${new_${site}}" contents "${contents}")
    endforeach()
    file(WRITE "${source}" "${contents}")
endfunction()
